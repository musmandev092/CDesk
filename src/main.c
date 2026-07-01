/* main.c — DankC entry point.
 *
 * Milestone 2: connect to niri via Wayland, bring up EGL + nanovg, and place a
 * themed bar with a live clock on every output. Later milestones add services,
 * more widgets, and the rest of the shell (see docs/06-ROADMAP.md).
 */
#include "core/log.h"
#include "core/loop.h"
#include "dc.h"
#include "niri/niri.h"
#include "render/nvg.h"
#include "services/bluez.h"
#include "services/dbus.h"
#include "services/audio.h"
#include "services/mpris.h"
#include "services/notifications.h"
#include "theme/theme.h"
#include "ui/bar/bar.h"
#include "ui/controlcenter.h"
#include "ui/osd.h"
#include "ui/toasts.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <poll.h>
#include <signal.h>
#include <stdlib.h>
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

/* Notification server signalled a change: rebuild the toast stack. */
static void notifications_changed(void *data)
{
    dc_toasts_refresh(data);
}

struct click_ctx {
    struct bar_set *set;
    dc_control_center *control_center;
    dc_toasts *toasts;
};

/* Route a left click: into the control-center popup if it's the target, else to
 * the bar under the pointer (toggle the control center, or dismiss it). */
static void handle_bar_click(struct wl_surface *surface, double x, double y, void *data)
{
    struct click_ctx *ctx = data;
    dc_control_center *cc = ctx->control_center;

    if (dc_toasts_handle_click(ctx->toasts, surface, x, y))
        return;

    if (dc_control_center_visible(cc) && surface == dc_control_center_surface(cc)) {
        dc_control_center_handle_click(cc, x, y);
        return;
    }

    for (int i = 0; i < ctx->set->count; i++) {
        dc_bar *bar = ctx->set->bars[i];
        if (dc_bar_surface(bar) != surface)
            continue;
        dc_bar_region region = dc_bar_hittest(bar, x, y);
        if (region == DC_BAR_REGION_CONTROL_CENTER)
            dc_control_center_toggle(cc, dc_bar_output(bar));
        else if (dc_control_center_visible(cc))
            dc_control_center_hide(cc);
        return;
    }
}

int main(void)
{
    dc_log_init(DC_LOG_DEBUG);
    dc_info("DankC %s starting", DC_VERSION);
    dc_theme_init();

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

    dc_control_center *control_center = dc_control_center_create(wl, &egl, &render);
    dc_osd *osd = dc_osd_create(wl, &egl, &render);

    dc_output *first_output = NULL;
    wl_list_for_each(first_output, &wl->outputs, link) {
        break;
    }
    dc_toasts *toasts = dc_toasts_create(wl, &egl, &render, notifications, first_output);
    dc_notifications_set_changed_cb(notifications, notifications_changed, toasts);

    struct click_ctx cctx = {.set = &set, .control_center = control_center, .toasts = toasts};
    struct tick_ctx tick = {.set = &set, .osd = osd, .wl = wl, .notifications = notifications};

    g_loop = dc_loop_create();
    dc_wayland_integrate(wl, g_loop);
    dc_niri_integrate(niri, g_loop);
    dc_niri_set_changed_cb(niri, niri_changed, &set);
    dc_wayland_set_click_cb(wl, handle_bar_click, &cctx);
dc_dbus_integrate(dbus, g_loop);
dc_osd_integrate(osd, g_loop);

    dc_loop_set_tick(g_loop, clock_tick, &tick, 1000);

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

    dc_info("entering event loop (%d bar%s)", set.count, set.count == 1 ? "" : "s");
    dc_loop_run(g_loop);

    dc_info("shutting down");
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
