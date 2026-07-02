/* main.c — DankC entry point.
 *
 * Milestone 2: connect to niri via Wayland, bring up EGL + nanovg, and place a
 * themed bar with a live clock on every output. Later milestones add services,
 * more widgets, and the rest of the shell (see docs/06-ROADMAP.md).
 */
#include "core/config.h"
#include "core/log.h"
#include "core/loop.h"
#include "ipc/control.h"
#include "dc.h"
#include "niri/niri.h"
#include "render/nvg.h"
#include "services/bluez.h"
#include "services/clipboard.h"
#include "services/dbus.h"
#include "services/audio.h"
#include "services/logind.h"
#include "services/mpris.h"
#include "services/notifications.h"
#include "services/sysmon.h"
#include "services/tray.h"
#include "services/weather.h"
#include "theme/theme.h"
#include "ui/bar/bar.h"
#include "ui/clip_picker.h"
#include "ui/controlcenter.h"
#include "ui/launcher.h"
#include "ui/lock.h"
#include "ui/notifcenter.h"
#include "ui/osd.h"
#include "ui/settings.h"
#include "ui/toasts.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define DC_MAX_BARS 16

static dc_loop *g_loop = NULL;

struct bar_set {
    dc_bar *bars[DC_MAX_BARS];
    int count;
};

static void handle_signal(int signum)
{
    DC_UNUSED(signum);
    if (g_loop)
        dc_loop_stop(g_loop);
}

static void render_all(struct bar_set *set)
{
    for (int i = 0; i < set->count; i++)
        dc_bar_render(set->bars[i]);
}

struct tick_ctx {
    struct bar_set *set;
    dc_osd *osd;
    dc_wayland *wl;
    dc_notifications *notifications;
    dc_lock *lock;
    int last_volume;
    bool last_muted;
    bool have_last;
};

/* Called ~once per second by the loop: redraw the bars (clock) and pop the
 * volume OSD on a change. */
static void clock_tick(void *data)
{
    struct tick_ctx *ctx = data;
    dc_notifications_tick(ctx->notifications);
    dc_lock_tick(ctx->lock);
    dc_sysmon_poll(); /* self-limits to 3s (docs/12-BAR-SPEC.md sec.4 cpuUsage/memUsage) */
    render_all(ctx->set);

    dc_audio_info audio;
    if (dc_audio_read(&audio) && audio.available) {
        if (ctx->have_last && (audio.volume != ctx->last_volume || audio.muted != ctx->last_muted)) {
            dc_output *first = NULL;
            wl_list_for_each(first, &ctx->wl->outputs, link) {
                break;
            }
            if (first)
                dc_osd_show_volume(ctx->osd, first, audio.volume, audio.muted);
        }
        ctx->last_volume = audio.volume;
        ctx->last_muted = audio.muted;
        ctx->have_last = true;
    }
}

/* Called when niri's workspace state changes. */
static void niri_changed(void *data)
{
    render_all(data);
}

/* The two notification views fed by the server's changed-callback. */
struct notif_ui {
    dc_toasts *toasts;
    dc_notif_center *center;
};

/* Notification server signalled a change: rebuild the toasts + center. */
static void notifications_changed(void *data)
{
    struct notif_ui *ui = data;
    dc_toasts_refresh(ui->toasts);
    dc_notif_center_refresh(ui->center);
}

struct click_ctx {
    struct bar_set *set;
    dc_control_center *control_center;
    dc_toasts *toasts;
    dc_launcher *launcher;
    dc_notif_center *notif_center;
    dc_clip_picker *clip_picker;
    dc_settings *settings;
    dc_notifications *notifications;
};

/* logind asked us to lock (pre-sleep / lock-session). */
static void logind_lock(void *data)
{
    dc_lock_engage(data);
}

/* Keyboard input routed to whichever keyboard-grabbing overlay is open. */
struct kbd_ctx {
    dc_launcher *launcher;
    dc_clip_picker *clip_picker;
    dc_lock *lock;
};

static void handle_key(uint32_t keysym, const char *utf8, void *data)
{
    struct kbd_ctx *k = data;
    if (dc_lock_active(k->lock)) /* lock takes all input while engaged */
        dc_lock_handle_key(k->lock, keysym, utf8);
    else if (dc_clip_picker_visible(k->clip_picker))
        dc_clip_picker_handle_key(k->clip_picker, keysym, utf8);
    else
        dc_launcher_handle_key(k->launcher, keysym, utf8);
}

/* State the control socket dispatches commands against. */
struct control_ctx {
    struct dc_wayland *wl;
    dc_launcher *launcher;
    dc_control_center *control_center;
    dc_notif_center *notif_center;
    dc_clip_picker *clip_picker;
    dc_settings *settings;
    dc_lock *lock;
    dc_notifications *notifications;
};

static struct dc_output *first_output(struct dc_wayland *wl)
{
    dc_output *o = NULL;
    wl_list_for_each(o, &wl->outputs, link) {
        return o;
    }
    return NULL;
}

/* Run a shell command detached (children auto-reaped via SIGCHLD SIG_IGN). */
static void run_sh(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* Map a dankctl command to a shell action. */
static void control_dispatch(const char *cmd, void *data)
{
    struct control_ctx *c = data;
    dc_output *out = first_output(c->wl);
    if (strcmp(cmd, "launcher") == 0 || strcmp(cmd, "launcher toggle") == 0)
        dc_launcher_toggle(c->launcher, out);
    else if (strcmp(cmd, "control-center") == 0 || strcmp(cmd, "control-center toggle") == 0)
        dc_control_center_toggle(c->control_center, out);
    else if (strcmp(cmd, "notifications") == 0 || strcmp(cmd, "notifications toggle") == 0) {
        dc_notif_center_toggle(c->notif_center, out);
        if (dc_notif_center_visible(c->notif_center))
            dc_notifications_mark_read(c->notifications);
    }
    else if (strcmp(cmd, "clipboard") == 0 || strcmp(cmd, "clipboard toggle") == 0)
        dc_clip_picker_toggle(c->clip_picker, out);
    else if (strcmp(cmd, "settings") == 0 || strcmp(cmd, "settings toggle") == 0)
        dc_settings_toggle(c->settings, out);
    else if (strcmp(cmd, "lock") == 0)
        dc_lock_engage(c->lock);
    else if (strcmp(cmd, "unlock") == 0 && getenv("DANKC_LOCK_ESCAPE"))
        dc_lock_force_unlock(c->lock); /* testing-only, env-gated */
    else if (strcmp(cmd, "screenshot") == 0)
        /* Full screen -> ~/Pictures + clipboard (needs grim + wl-copy). */
        run_sh("f=\"${XDG_PICTURES_DIR:-$HOME/Pictures}/screenshot-$(date +%Y%m%d-%H%M%S).png\"; "
               "mkdir -p \"$(dirname \"$f\")\"; grim \"$f\" && wl-copy --type image/png < \"$f\"");
    else if (strcmp(cmd, "screenshot-region") == 0)
        /* Interactive region -> file + clipboard (needs slurp). */
        run_sh("f=\"${XDG_PICTURES_DIR:-$HOME/Pictures}/screenshot-$(date +%Y%m%d-%H%M%S).png\"; "
               "mkdir -p \"$(dirname \"$f\")\"; g=$(slurp) || exit; grim -g \"$g\" \"$f\" && "
               "wl-copy --type image/png < \"$f\"");
    else if (strcmp(cmd, "color-picker") == 0)
        /* Pick a pixel -> #rrggbb to clipboard (slurp point + grim PPM). */
        run_sh("g=$(slurp -p) || exit; px=$(grim -g \"$g\" -t ppm - | tail -c3 | od -An -tu1); "
               "set -- $px; printf '#%02x%02x%02x' \"$1\" \"$2\" \"$3\" | wl-copy");
    else if (strcmp(cmd, "night") == 0)
        /* Toggle a warm night filter (gammastep one-shot). */
        run_sh("if pgrep -x gammastep >/dev/null; then pkill -x gammastep; "
               "else gammastep -O 4000 >/dev/null 2>&1 & fi");
    else if (strcmp(cmd, "quit") == 0)
        dc_loop_stop(g_loop);
    else
        dc_warn("unknown control command: %s", cmd);
}

/* `dankc ctl <words...>` — send a command to the running shell. */
static int run_client(int argc, char **argv)
{
    char cmd[512] = {0};
    size_t len = 0;
    for (int i = 2; i < argc && len < sizeof(cmd) - 1; i++) {
        int w = snprintf(cmd + len, sizeof(cmd) - len, "%s%s", len ? " " : "", argv[i]);
        if (w > 0)
            len += (size_t)w;
    }
    return dc_control_send(cmd) == 0 ? 0 : 1;
}

/* `dankc keybinds` — print a niri config snippet for the control commands. */
static void print_keybinds(void)
{
    printf("// DankC keybinds — add inside the binds { } block of ~/.config/niri/config.kdl\n"
           "    Mod+D            { spawn \"dankc\" \"ctl\" \"launcher\"; }\n"
           "    Mod+N            { spawn \"dankc\" \"ctl\" \"notifications\"; }\n"
           "    Mod+Shift+C      { spawn \"dankc\" \"ctl\" \"control-center\"; }\n"
           "    Mod+V            { spawn \"dankc\" \"ctl\" \"clipboard\"; }\n"
           "    Print            { spawn \"dankc\" \"ctl\" \"screenshot\"; }\n"
           "    Mod+Print        { spawn \"dankc\" \"ctl\" \"screenshot-region\"; }\n"
           "    Mod+Shift+P      { spawn \"dankc\" \"ctl\" \"color-picker\"; }\n"
           "    Mod+Shift+N      { spawn \"dankc\" \"ctl\" \"night\"; }\n"
           "    Mod+Comma        { spawn \"dankc\" \"ctl\" \"settings\"; }\n");
}

/* Route a left click: into the control-center popup if it's the target, else to
 * the bar under the pointer (toggle the control center, or dismiss it). */
static void handle_bar_click(struct wl_surface *surface, double x, double y, void *data)
{
    struct click_ctx *ctx = data;
    dc_control_center *cc = ctx->control_center;

    if (dc_toasts_handle_click(ctx->toasts, surface, x, y))
        return;

    if (dc_launcher_visible(ctx->launcher) && surface == dc_launcher_surface(ctx->launcher)) {
        dc_launcher_handle_click(ctx->launcher, x, y);
        return;
    }

    if (dc_control_center_visible(cc) && surface == dc_control_center_surface(cc)) {
        dc_control_center_handle_click(cc, x, y);
        return;
    }

    if (dc_notif_center_visible(ctx->notif_center) &&
        surface == dc_notif_center_surface(ctx->notif_center)) {
        dc_notif_center_handle_click(ctx->notif_center, x, y);
        return;
    }

    if (dc_clip_picker_visible(ctx->clip_picker) &&
        surface == dc_clip_picker_surface(ctx->clip_picker)) {
        dc_clip_picker_handle_click(ctx->clip_picker, x, y);
        return;
    }

    if (dc_settings_visible(ctx->settings) && surface == dc_settings_surface(ctx->settings)) {
        dc_settings_handle_click(ctx->settings, x, y);
        return;
    }

    for (int i = 0; i < ctx->set->count; i++) {
        dc_bar *bar = ctx->set->bars[i];
        if (dc_bar_surface(bar) != surface)
            continue;
        int payload = 0;
        dc_bar_region region = dc_bar_hittest(bar, x, y, &payload);
        if (region == DC_BAR_REGION_LAUNCHER) {
            dc_launcher_toggle(ctx->launcher, dc_bar_output(bar));
        } else if (region == DC_BAR_REGION_NOTIFICATIONS) {
            dc_notif_center_toggle(ctx->notif_center, dc_bar_output(bar));
            if (dc_notif_center_visible(ctx->notif_center))
                dc_notifications_mark_read(ctx->notifications);
        } else if (region == DC_BAR_REGION_CLIPBOARD) {
            dc_clip_picker_toggle(ctx->clip_picker, dc_bar_output(bar));
        } else if (region == DC_BAR_REGION_CONTROL_CENTER) {
            dc_control_center_toggle(cc, dc_bar_output(bar));
        } else if (region == DC_BAR_REGION_WORKSPACE) {
            dc_niri_focus_workspace(payload);
        } else if (region == DC_BAR_REGION_MEDIA_PREV) {
            dc_mpris_previous();
        } else if (region == DC_BAR_REGION_MEDIA_PLAY) {
            dc_mpris_play_pause();
        } else if (region == DC_BAR_REGION_MEDIA_NEXT) {
            dc_mpris_next();
        } else {
            if (dc_control_center_visible(cc))
                dc_control_center_hide(cc);
            if (dc_notif_center_visible(ctx->notif_center))
                dc_notif_center_hide(ctx->notif_center);
        }
        return;
    }
}

/* Pointer motion over a bar surface: forward to hover tracking, which
 * internally re-renders only on a hover-region change (docs/12-BAR-SPEC.md
 * sec.5). Other DankC surfaces (popups, launcher) don't have per-pixel hover
 * yet, so a miss here is a silent no-op. */
static void handle_bar_motion(struct wl_surface *surface, double x, double y, void *data)
{
    struct bar_set *set = data;
    for (int i = 0; i < set->count; i++) {
        if (dc_bar_surface(set->bars[i]) == surface) {
            dc_bar_pointer_motion(set->bars[i], x, y);
            return;
        }
    }
}

/* Pointer left a surface entirely: clear hover on whichever bar it was (if
 * any — popups don't track hover). */
static void handle_bar_leave(struct wl_surface *surface, void *data)
{
    struct bar_set *set = data;
    for (int i = 0; i < set->count; i++) {
        if (dc_bar_surface(set->bars[i]) == surface) {
            dc_bar_pointer_leave(set->bars[i]);
            return;
        }
    }
}

/* Scroll on a bar surface: vertical wheel -> workspace focus, horizontal ->
 * column focus (docs/12-BAR-SPEC.md sec.5 — the user's live config,
 * scrollYBehavior=workspace/scrollXBehavior=column; other behavior modes
 * aren't wired up, since dankc's config doesn't expose them yet). `steps_v`/
 * `steps_h` are already debounced into whole steps by dc_wayland (wl.c); one
 * niri action fires per callback regardless of step magnitude, so a single
 * fast flick can't spawn a pile of `niri msg` processes. */
static void handle_bar_axis(struct wl_surface *surface, int steps_v, int steps_h, void *data)
{
    struct bar_set *set = data;
    bool on_bar = false;
    for (int i = 0; i < set->count; i++) {
        if (dc_bar_surface(set->bars[i]) == surface) {
            on_bar = true;
            break;
        }
    }
    if (!on_bar)
        return;

    if (steps_v > 0)
        dc_niri_focus_workspace_down();
    else if (steps_v < 0)
        dc_niri_focus_workspace_up();

    if (steps_h > 0)
        dc_niri_focus_column_right();
    else if (steps_h < 0)
        dc_niri_focus_column_left();
}

int main(int argc, char **argv)
{
    /* Client modes exit immediately without starting the shell. */
    if (argc >= 2 && strcmp(argv[1], "ctl") == 0)
        return run_client(argc, argv);
    if (argc >= 2 && strcmp(argv[1], "keybinds") == 0) {
        print_keybinds();
        return 0;
    }

    dc_log_init(DC_LOG_DEBUG);
    dc_info("DankC %s starting", DC_VERSION);
    dc_theme_init();
    dc_config_load();

    dc_wayland *wl = dc_wayland_connect();
    if (!wl)
        return 1;

    dc_egl egl = {0};
    if (!dc_egl_init(&egl, wl->display)) {
        dc_wayland_destroy(wl);
        return 1;
    }

    dc_niri *niri = dc_niri_connect();
    dc_dbus *dbus = dc_dbus_connect();
    dc_bluez_init(dbus);
    dc_mpris_init(dbus);
    dc_notifications *notifications = dc_notifications_create(dbus);
    dc_tray *tray = dc_tray_create(dbus);

    /* Bar weather widget (docs/12-BAR-SPEC.md sec.4): only arm the fetch loop
     * when enabled — dc_weather_get() just reports "no reading yet" (widget
     * hidden) if dc_weather_init() is never called. */
    if (dc_config_current->weather_enabled)
        dc_weather_init(dc_config_current->weather_lat, dc_config_current->weather_lon,
                        dc_config_current->weather_fahrenheit);

    dc_render render = {0};
    struct bar_set set = {0};
    dc_output *output;
    wl_list_for_each(output, &wl->outputs, link) {
        if (set.count >= DC_MAX_BARS) {
            dc_warn("more than %d outputs; ignoring the rest", DC_MAX_BARS);
            break;
        }
        set.bars[set.count++] = dc_bar_create(wl, output, &egl, &render, niri);
    }
    if (set.count == 0)
        dc_warn("no outputs found; nothing to display");

    for (int i = 0; i < set.count; i++) {
        dc_bar_set_tray(set.bars[i], tray);
        dc_bar_set_notifications(set.bars[i], notifications);
    }
    dc_tray_set_changed_cb(tray, niri_changed, &set); /* re-render bars on tray change */

    dc_control_center *control_center = dc_control_center_create(wl, &egl, &render);
    dc_osd *osd = dc_osd_create(wl, &egl, &render);

    dc_output *first_output = NULL;
    wl_list_for_each(first_output, &wl->outputs, link) {
        break;
    }
    dc_toasts *toasts = dc_toasts_create(wl, &egl, &render, notifications, first_output);
    dc_notif_center *notif_center = dc_notif_center_create(wl, &egl, &render, notifications);
    struct notif_ui notif_ui = {.toasts = toasts, .center = notif_center};
    dc_notifications_set_changed_cb(notifications, notifications_changed, &notif_ui);

    dc_launcher *launcher = dc_launcher_create(wl, &egl, &render);
    dc_lock *lock = dc_lock_create(wl, &egl, &render);
    struct tick_ctx tick = {
        .set = &set, .osd = osd, .wl = wl, .notifications = notifications, .lock = lock};

    g_loop = dc_loop_create();
    dc_wayland_integrate(wl, g_loop);
    dc_niri_integrate(niri, g_loop);
    dc_niri_set_changed_cb(niri, niri_changed, &set);
    dc_dbus_integrate(dbus, g_loop);
    dc_osd_integrate(osd, g_loop);
    dc_loop_set_tick(g_loop, clock_tick, &tick, 1000);

    dc_clipboard *clipboard = dc_clipboard_create(wl, g_loop);
    dc_clip_picker *clip_picker = dc_clip_picker_create(wl, &egl, &render, clipboard);
    dc_settings *settings = dc_settings_create(wl, &egl, &render);

    struct kbd_ctx kbd = {.launcher = launcher, .clip_picker = clip_picker, .lock = lock};
    dc_wayland_set_key_cb(wl, handle_key, &kbd);

    struct click_ctx cctx = {.set = &set,
                             .control_center = control_center,
                             .toasts = toasts,
                             .launcher = launcher,
                             .notif_center = notif_center,
                             .clip_picker = clip_picker,
                             .settings = settings,
                             .notifications = notifications};
    dc_wayland_set_click_cb(wl, handle_bar_click, &cctx);
    dc_wayland_set_motion_cb(wl, handle_bar_motion, &set);
    dc_wayland_set_leave_cb(wl, handle_bar_leave, &set);
    dc_wayland_set_axis_cb(wl, handle_bar_axis, &set);

    struct control_ctx control_ctx = {.wl = wl,
                                      .launcher = launcher,
                                      .control_center = control_center,
                                      .notif_center = notif_center,
                                      .clip_picker = clip_picker,
                                      .settings = settings,
                                      .lock = lock,
                                      .notifications = notifications};
    dc_control *control = dc_control_create(g_loop, control_dispatch, &control_ctx);
    dc_logind *logind = dc_logind_create(dbus, logind_lock, lock);

    struct sigaction sa = {.sa_handler = handle_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    signal(SIGCHLD, SIG_IGN); /* auto-reap detached action processes */

    /* TEMP(verify): auto-open a popup to screenshot it. */
    if (getenv("DANKC_CC_DEMO") || getenv("DANKC_OSD_DEMO")) {
        dc_output *first = NULL;
        wl_list_for_each(first, &wl->outputs, link) {
            break;
        }
        if (first && getenv("DANKC_CC_DEMO"))
            dc_control_center_toggle(control_center, first);
        if (first && getenv("DANKC_OSD_DEMO"))
            dc_osd_show_volume(osd, first, 55, false);
    }
    if (getenv("DANKC_LAUNCHER_DEMO")) {
        dc_output *first = NULL;
        wl_list_for_each(first, &wl->outputs, link) {
            break;
        }
        dc_launcher_toggle(launcher, first);
    }
    if (getenv("DANKC_NC_DEMO")) {
        dc_output *first = NULL;
        wl_list_for_each(first, &wl->outputs, link) {
            break;
        }
        dc_notif_center_toggle(notif_center, first);
    }

    dc_info("entering event loop (%d bar%s)", set.count, set.count == 1 ? "" : "s");
    dc_loop_run(g_loop);

    dc_info("shutting down");
    dc_logind_destroy(logind);
    dc_tray_destroy(tray);
    dc_lock_destroy(lock);
    dc_settings_destroy(settings);
    dc_clip_picker_destroy(clip_picker);
    dc_clipboard_destroy(clipboard);
    dc_control_destroy(control);
    dc_launcher_destroy(launcher);
    dc_notif_center_destroy(notif_center);
    dc_toasts_destroy(toasts);
    dc_notifications_destroy(notifications);
    dc_osd_destroy(osd);
    dc_control_center_destroy(control_center);
    for (int i = 0; i < set.count; i++)
        dc_bar_destroy(set.bars[i]);
    /* GL teardown is skipped: the process is exiting and nvgDelete needs a live
     * context; the driver reclaims resources on exit. */
    dc_loop_destroy(g_loop);
    dc_dbus_destroy(dbus);
    dc_niri_destroy(niri);
    dc_egl_finish(&egl);
    dc_wayland_destroy(wl);
    return 0;
}
