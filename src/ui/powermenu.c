/* powermenu.c — session power menu modal.
 *
 * Reference: DankMaterialShell's Modals/PowerMenuModal.qml (DankModal-based,
 * positioning "center" when opened without a parent anchor, which is how
 * dankc always opens it — from `dankc ctl power-menu` or a keybind, never
 * from inside another popout). Default (non-grid) layout there is a vertical
 * Column of 56px-tall rows; that's what this file draws. DMS's default
 * action set/order is ["reboot","logout","poweroff","lock","suspend",
 * "restart"] (restart = "restart DMS", not applicable here) — this task's
 * brief pins the five session actions to Lock, Logout, Suspend, Reboot,
 * Shutdown in that order, which is what's implemented below.
 *
 * CONFIRMATION — deviation from DMS, and why:
 * DMS's actionNeedsConfirm() requires a *continuous hold* (0.5s, animated
 * progress bar) on every action except Lock/restart-DMS, driven by a 16ms
 * Timer plus onPressed/onReleased/onCanceled from a QML MouseArea. dankc's
 * Wayland keyboard plumbing (wayland/wl.c's keyboard_handle_key) only
 * forwards KEY_STATE_PRESSED to the key callback -- releases never reach
 * application code, and there's no client-side key-repeat timer -- so a
 * held Enter key cannot be distinguished from a single tap. A hold gesture
 * is thus only implementable for the mouse (press+release both propagate;
 * see wayland/wl.c's click/release callbacks), and a mouse-only hold with a
 * keyboard-only tap-twice would be an inconsistent, confusing UI. Instead,
 * every destructive action (everything but Lock) requires two activations
 * of the *same* row in a row ("arm" then "confirm") for both input methods:
 * first Enter/click on Logout/Suspend/Reboot/Shutdown arms it (row turns
 * warning/error-tinted, hint row explains it), a second Enter/click on the
 * still-armed row fires it. Selecting a different row, or Escape, disarms.
 * This is exactly the "are-you-sure sub-state" the task brief calls out as
 * an acceptable substitute for a literal hold gesture.
 */
#include "ui/powermenu.h"

#include "core/anim.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/dbus.h"
#include "theme/theme.h"
#include "ui/lock.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_SCALE_BASE 120

/* Card geometry (logical px) — sized for 5 rows + hint row, matching
 * PowerMenuModal.qml's `modalWidth: ... : 400` (list/non-grid branch). */
#define DC_PM_WIDTH 400.0f
#define DC_PM_PAD 6.0f    /* shadow room around the rounded card */
#define DC_PM_INSET 16.0f /* Theme.spacingL: card edge -> row column */
#define DC_PM_TOP 16.0f
#define DC_PM_ROW_H 56.0f
#define DC_PM_ROW_GAP 8.0f
#define DC_PM_HINT_H 26.0f
#define DC_PM_BOTTOM 16.0f

typedef enum {
    DC_PM_LOCK = 0,
    DC_PM_LOGOUT,
    DC_PM_SUSPEND,
    DC_PM_REBOOT,
    DC_PM_SHUTDOWN,
    DC_PM_ACTION_COUNT,
} dc_pm_action;

/* warn: 0 = normal, 1 = warning (orange), 2 = error (red) — matches DMS's
 * `showWarning = modelData === "reboot" || modelData === "poweroff"` plus
 * its poweroff-vs-reboot color split (Theme.error vs Theme.warning). */
typedef struct {
    const char *label;
    int icon;
    bool needs_confirm;
    int warn;
} pm_action_def;

static const pm_action_def PM_ACTIONS[DC_PM_ACTION_COUNT] = {
    [DC_PM_LOCK] = {"Lock", DC_ICON_LOCK, false, 0},
    [DC_PM_LOGOUT] = {"Log Out", DC_ICON_LOGOUT, true, 0},
    [DC_PM_SUSPEND] = {"Suspend", DC_ICON_BEDTIME, true, 0},
    [DC_PM_REBOOT] = {"Reboot", DC_ICON_RESTART_ALT, true, 1},
    [DC_PM_SHUTDOWN] = {"Shutdown", DC_ICON_POWER, true, 2},
};

#define DC_PM_HEIGHT                                                                             \
    (2.0f * DC_PM_PAD + DC_PM_TOP + DC_PM_ACTION_COUNT * DC_PM_ROW_H +                            \
     (DC_PM_ACTION_COUNT - 1) * DC_PM_ROW_GAP + DC_PM_HINT_H + DC_PM_BOTTOM)

struct dc_powermenu {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;
    struct dc_dbus *dbus; /* system bus for login1 (may be NULL) */
    struct dc_lock *lock; /* reused for the Lock action */

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width, logical_height; /* full output — this surface fills it */
    int scale120;
    int phys_width, phys_height;

    int selected; /* keyboard/hover highlight, always valid (0..COUNT-1) */
    int armed;    /* row pending a second confirm, or -1 */

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;

    bool visible;
    bool configured;
    bool egl_ready;
};

/* Card + row placement, recomputed every render from the (whole-output)
 * surface size — same "shared layout struct" convention as launcher.c's
 * launcher_layout / clip_picker.c's cp_layout. */
typedef struct {
    float card_x, card_y, card_w, card_h;
    float row_x, row_w;
    float row_y[DC_PM_ACTION_COUNT];
    float hint_y;
} pm_layout;

static pm_layout pm_get_layout(float screen_w, float screen_h)
{
    pm_layout lay;
    lay.card_w = DC_PM_WIDTH;
    lay.card_h = DC_PM_HEIGHT;
    lay.card_x = (screen_w - lay.card_w) / 2.0f;
    lay.card_y = (screen_h - lay.card_h) / 2.0f;
    lay.row_x = lay.card_x + DC_PM_PAD + DC_PM_INSET;
    lay.row_w = lay.card_w - 2.0f * DC_PM_PAD - 2.0f * DC_PM_INSET;
    float y = lay.card_y + DC_PM_PAD + DC_PM_TOP;
    for (int i = 0; i < DC_PM_ACTION_COUNT; i++) {
        lay.row_y[i] = y;
        y += DC_PM_ROW_H + DC_PM_ROW_GAP;
    }
    lay.hint_y = lay.card_y + lay.card_h - DC_PM_PAD - DC_PM_BOTTOM - DC_PM_HINT_H / 2.0f;
    return lay;
}

static void pm_render(dc_powermenu *pm);
static void pm_teardown(dc_powermenu *pm);

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_powermenu *pm = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    pm->frame_cb = NULL;
    if (!pm->visible)
        return;
    if (dc_anim_active(&pm->anim)) {
        pm_render(pm);
    } else if (pm->closing) {
        pm_teardown(pm);
    }
}
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}
static inline dc_color dc_alpha(dc_color c, int a)
{
    c.a = (uint8_t)a;
    return c;
}
static inline bool in_rect(double x, double y, float x0, float y0, float x1, float y1)
{
    return x1 > x0 && x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

static const dc_color *pm_warn_color(const dc_theme *t, int warn)
{
    if (warn == 2)
        return &t->error;
    if (warn == 1)
        return &t->warning;
    return &t->primary;
}

static void recompute_physical(dc_powermenu *pm)
{
    pm->phys_width = (pm->logical_width * pm->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    pm->phys_height = (pm->logical_height * pm->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* --- actions ------------------------------------------------------------ */

/* Run a shell command detached (children auto-reaped via SIGCHLD SIG_IGN in
 * main.c) — same fork+setsid+execl pattern as controlcenter.c's
 * run_detached(). */
static void pm_run_detached(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* org.freedesktop.login1.Manager.<method>(false) on the system bus — same
 * dest/path/interface + sd_bus_call_method() style as services/logind.c and
 * services/mpris.c's find_player(). `interactive=false` (the single boolean
 * arg) matches DMS's SessionService.suspend()/reboot()/poweroff(), which
 * shell out to loginctl/systemctl non-interactively. */
static void pm_login1_call(dc_powermenu *pm, const char *method)
{
    if (getenv("DANKC_POWER_DRYRUN")) {
        dc_info("[DRYRUN] power-menu: would call org.freedesktop.login1 "
                "/org/freedesktop/login1 org.freedesktop.login1.Manager.%s(interactive=false)",
                method);
        return;
    }
    if (!pm->dbus || !pm->dbus->system) {
        dc_warn("power-menu: no system bus; cannot call login1.%s", method);
        return;
    }
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(pm->dbus->system, "org.freedesktop.login1", "/org/freedesktop/login1",
                               "org.freedesktop.login1.Manager", method, &err, NULL, "b", 0);
    if (r < 0)
        dc_warn("power-menu: login1.%s failed: %s", method, err.message ? err.message : "unknown");
    sd_bus_error_free(&err);
}

/* Fire the (already-confirmed) action at `idx`. */
static void pm_fire(dc_powermenu *pm, int idx)
{
    bool dry = getenv("DANKC_POWER_DRYRUN") != NULL;
    switch ((dc_pm_action)idx) {
    case DC_PM_LOCK:
        if (dry)
            dc_info("[DRYRUN] power-menu: would engage session lock (dc_lock_engage)");
        else if (pm->lock)
            dc_lock_engage(pm->lock);
        break;
    case DC_PM_LOGOUT:
        if (dry)
            dc_info("[DRYRUN] power-menu: would spawn detached: niri msg action quit "
                    "--skip-confirmation");
        else
            pm_run_detached("niri msg action quit --skip-confirmation");
        break;
    case DC_PM_SUSPEND:
        pm_login1_call(pm, "Suspend");
        break;
    case DC_PM_REBOOT:
        pm_login1_call(pm, "Reboot");
        break;
    case DC_PM_SHUTDOWN:
        pm_login1_call(pm, "PowerOff");
        break;
    case DC_PM_ACTION_COUNT:
        break;
    }
}

/* --- render --------------------------------------------------------------
 */

static void draw_row(dc_powermenu *pm, const pm_layout *lay, int idx)
{
    NVGcontext *vg = pm->render->vg;
    const dc_theme *t = dc_theme_current;
    const pm_action_def *def = &PM_ACTIONS[idx];
    float x = lay->row_x, y = lay->row_y[idx], w = lay->row_w, h = DC_PM_ROW_H;
    bool selected = pm->selected == idx;
    bool armed = pm->armed == idx;
    const dc_color *warn_c = pm_warn_color(t, def->warn);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    if (armed) {
        nvgFillColor(vg, tc_alpha(*warn_c, def->warn ? 90 : 60));
    } else if (selected) {
        nvgFillColor(vg, tc_alpha(t->primary, 46));
    } else {
        nvgFillColor(vg, tc_alpha(t->surface_variant, 40));
    }
    nvgFill(vg);
    if (selected || armed) {
        nvgStrokeColor(vg, tc(armed ? *warn_c : t->primary));
        nvgStrokeWidth(vg, 2.0f);
        nvgStroke(vg);
    }

    dc_color icon_col = (armed || (def->warn && selected)) ? *warn_c : t->surface_text;
    float cy = y + h / 2.0f;
    dc_render_icon(pm->render, def->icon, x + 20.0f, cy, 22.0f, icon_col,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    nvgFontFaceId(vg, pm->render->font_ui);
    nvgFontSize(vg, 15.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(icon_col));
    nvgText(vg, x + 44.0f, cy, def->label, NULL);

    if (armed) {
        nvgFontSize(vg, 12.0f);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(*warn_c));
        nvgText(vg, x + w - 14.0f, cy, "Confirm?", NULL);
    }
}

static void draw_hint(dc_powermenu *pm, const pm_layout *lay)
{
    NVGcontext *vg = pm->render->vg;
    const dc_theme *t = dc_theme_current;
    float cx = lay->card_x + lay->card_w / 2.0f;

    const char *text;
    dc_color col;
    if (pm->armed >= 0) {
        text = "Press again / click again to confirm";
        col = *pm_warn_color(t, PM_ACTIONS[pm->armed].warn);
    } else {
        text = "\xe2\x86\x91\xe2\x86\x93 navigate \xc2\xb7 Enter select \xc2\xb7 Esc close";
        col = dc_alpha(t->surface_text, 130);
    }

    nvgFontFaceId(vg, pm->render->font_ui);
    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(col));
    nvgText(vg, cx, lay->hint_y, text, NULL);
}

static void pm_render(dc_powermenu *pm)
{
    if (!pm->configured || pm->phys_width <= 0)
        return;
    if (!pm->egl_ready) {
        if (!dc_egl_window_init(&pm->egl_window, pm->egl, pm->surface, pm->phys_width,
                                pm->phys_height))
            return;
        pm->egl_ready = true;
    } else {
        dc_egl_window_resize(&pm->egl_window, pm->phys_width, pm->phys_height);
    }
    if (!dc_egl_make_current(pm->egl, &pm->egl_window))
        return;
    if (!dc_render_ensure(pm->render))
        return;
    if (pm->viewport)
        wp_viewport_set_destination(pm->viewport, pm->logical_width, pm->logical_height);

    NVGcontext *vg = pm->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = (float)pm->logical_width, h = (float)pm->logical_height;

    glViewport(0, 0, pm->phys_width, pm->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)pm->scale120 / DC_SCALE_BASE);

    /* Entrance/exit: fade + scale about the card's own center (DMS's
     * DankModal does a plain opacity/scale pop, no directional origin, since
     * it's centered rather than bar-adjacent — see launcher.c for the
     * bar-adjacent-origin variant this mirrors). */
    float p = dc_anim_progress(&pm->anim);
    if (pm->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : (p < 0.0f ? 0.0f : p);
    float scale = 0.92f + 0.08f * (p > 1.0f ? 1.0f : p);

    pm_layout lay = pm_get_layout(w, h);
    float pivot_x = lay.card_x + lay.card_w / 2.0f;
    float pivot_y = lay.card_y + lay.card_h / 2.0f;

    /* Scrim: dim the whole screen behind the card (DMS: black @ 0.5 opacity
     * — Modals/Common/DankModalStandalone.qml's backgroundOpacity). Fades in
     * with the same alpha as the card itself. */
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgFillColor(vg, nvgRGBA(0, 0, 0, (unsigned char)(alpha * 128.0f)));
    nvgFill(vg);

    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, pivot_x, pivot_y);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -pivot_x, -pivot_y);

    /* Drop shadow + card. */
    NVGpaint shadow =
        nvgBoxGradient(vg, lay.card_x, lay.card_y + 2.0f, lay.card_w, lay.card_h, 16.0f, 24.0f,
                       nvgRGBA(0, 0, 0, 130), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, lay.card_x - 40.0f, lay.card_y - 40.0f, lay.card_w + 80.0f, lay.card_h + 80.0f);
    nvgRoundedRect(vg, lay.card_x, lay.card_y, lay.card_w, lay.card_h, 18.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, lay.card_x, lay.card_y, lay.card_w, lay.card_h, 18.0f);
    nvgFillColor(vg, tc(t->surface_container));
    nvgFill(vg);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    for (int i = 0; i < DC_PM_ACTION_COUNT; i++)
        draw_row(pm, &lay, i);
    draw_hint(pm, &lay);

    nvgEndFrame(vg);

    if ((dc_anim_active(&pm->anim) || pm->closing) && !pm->frame_cb) {
        pm->frame_cb = wl_surface_frame(pm->surface);
        wl_callback_add_listener(pm->frame_cb, &frame_listener, pm);
    }
    dc_egl_swap(pm->egl, &pm->egl_window);
}

/* --- surface lifecycle ---------------------------------------------------
 */

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_powermenu *pm = data;
    DC_UNUSED(fs);
    pm->scale120 = (int)scale;
    recompute_physical(pm);
    pm_render(pm);
}
static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_powermenu *pm = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    pm->logical_width = width > 0 ? (int)width : (int)DC_PM_WIDTH;
    pm->logical_height = height > 0 ? (int)height : (int)DC_PM_HEIGHT;
    pm->configured = true;
    recompute_physical(pm);
    pm_render(pm);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_powermenu *pm = data;
    DC_UNUSED(surface);
    pm->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_powermenu *dc_powermenu_create(dc_wayland *wl, dc_egl *egl, dc_render *render, struct dc_dbus *dbus,
                                  struct dc_lock *lock)
{
    dc_powermenu *pm = calloc(1, sizeof(*pm));
    pm->wl = wl;
    pm->egl = egl;
    pm->render = render;
    pm->dbus = dbus;
    pm->lock = lock;
    pm->scale120 = DC_SCALE_BASE;
    pm->selected = DC_PM_LOCK;
    pm->armed = -1;
    return pm;
}

static void pm_show(dc_powermenu *pm, dc_output *output)
{
    pm->output = output;
    pm->configured = false;
    pm->egl_ready = false;
    pm->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    pm->selected = DC_PM_LOCK;
    pm->armed = -1;
    dc_anim_start(&pm->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    pm->surface = wl_compositor_create_surface(pm->wl->compositor);
    if (pm->wl->fractional_scale_mgr) {
        pm->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            pm->wl->fractional_scale_mgr, pm->surface);
        wp_fractional_scale_v1_add_listener(pm->fractional_scale, &fractional_scale_listener, pm);
    }
    if (pm->wl->viewporter)
        pm->viewport = wp_viewporter_get_viewport(pm->wl->viewporter, pm->surface);

    pm->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        pm->wl->layer_shell, pm->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:power-menu");

    /* Fill the whole output (all 4 anchors, size 0,0 -> compositor stretches
     * to fit) so the scrim can dim everything behind the centered card and a
     * click anywhere outside it can dismiss the menu, matching DMS's
     * DankModal (separate click-catcher layer + centered content layer) in
     * a single surface. */
    zwlr_layer_surface_v1_set_anchor(pm->layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                                            ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(pm->layer_surface, 0, 0);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        pm->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(pm->layer_surface, &layer_surface_listener, pm);

    wl_surface_commit(pm->surface);
    pm->visible = true;
    pm->closing = false;
    dc_debug("power-menu shown");
}

static void pm_teardown(dc_powermenu *pm)
{
    if (pm->frame_cb) {
        wl_callback_destroy(pm->frame_cb);
        pm->frame_cb = NULL;
    }
    if (pm->egl_ready)
        dc_egl_window_finish(&pm->egl_window, pm->egl);
    if (pm->viewport)
        wp_viewport_destroy(pm->viewport);
    if (pm->fractional_scale)
        wp_fractional_scale_v1_destroy(pm->fractional_scale);
    if (pm->layer_surface)
        zwlr_layer_surface_v1_destroy(pm->layer_surface);
    if (pm->surface)
        wl_surface_destroy(pm->surface);
    pm->egl_ready = false;
    pm->configured = false;
    pm->viewport = NULL;
    pm->fractional_scale = NULL;
    pm->layer_surface = NULL;
    pm->surface = NULL;
    pm->visible = false;
    pm->closing = false;
    dc_debug("power-menu hidden");
}

static void pm_begin_close(dc_powermenu *pm)
{
    if (!pm->visible || pm->closing)
        return;
    dc_anim_start(&pm->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    pm->closing = true;
    if (!dc_anim_active(&pm->anim)) {
        pm_teardown(pm);
        return;
    }
    pm_render(pm);
}

void dc_powermenu_toggle(dc_powermenu *pm, dc_output *output)
{
    if (pm->visible)
        pm_begin_close(pm);
    else
        pm_show(pm, output);
}

void dc_powermenu_hide(dc_powermenu *pm)
{
    pm_begin_close(pm);
}

bool dc_powermenu_visible(dc_powermenu *pm)
{
    return pm->visible;
}

struct wl_surface *dc_powermenu_surface(dc_powermenu *pm)
{
    return pm->surface;
}

/* Activate `idx`: destructive actions arm on the first activation (require a
 * second, matching activation to actually fire — see the file header for why
 * this replaces DMS's hold gesture); Lock fires immediately. */
static void pm_activate(dc_powermenu *pm, int idx)
{
    if (idx < 0 || idx >= DC_PM_ACTION_COUNT)
        return;
    pm->selected = idx;
    if (PM_ACTIONS[idx].needs_confirm && pm->armed != idx) {
        pm->armed = idx;
        pm_render(pm);
        return;
    }
    pm->armed = -1;
    pm_fire(pm, idx);
    pm_begin_close(pm);
}

void dc_powermenu_handle_key(dc_powermenu *pm, uint32_t keysym, const char *utf8)
{
    DC_UNUSED(utf8);
    if (!pm->visible || pm->closing)
        return;

    switch (keysym) {
    case XKB_KEY_Escape:
        if (pm->armed >= 0) {
            pm->armed = -1;
            pm_render(pm);
        } else {
            pm_begin_close(pm);
        }
        return;
    case XKB_KEY_Up:
        pm->selected = (pm->selected - 1 + DC_PM_ACTION_COUNT) % DC_PM_ACTION_COUNT;
        pm->armed = -1;
        pm_render(pm);
        return;
    case XKB_KEY_Down:
        pm->selected = (pm->selected + 1) % DC_PM_ACTION_COUNT;
        pm->armed = -1;
        pm_render(pm);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        pm_activate(pm, pm->selected);
        return;
    default:
        return;
    }
}

/* Row index at logical (x, y), or -1 if not over any row. */
static int pm_row_at(const pm_layout *lay, double x, double y)
{
    for (int i = 0; i < DC_PM_ACTION_COUNT; i++) {
        if (in_rect(x, y, lay->row_x, lay->row_y[i], lay->row_x + lay->row_w,
                    lay->row_y[i] + DC_PM_ROW_H))
            return i;
    }
    return -1;
}

void dc_powermenu_handle_click(dc_powermenu *pm, double x, double y)
{
    if (!pm->visible || pm->closing)
        return;

    pm_layout lay = pm_get_layout((float)pm->logical_width, (float)pm->logical_height);
    int idx = pm_row_at(&lay, x, y);
    if (idx >= 0) {
        pm_activate(pm, idx);
        return;
    }

    /* Outside the card entirely -> background click, dismiss (DMS's
     * onBackgroundClicked). Inside the card but not on a row is a no-op. */
    bool in_card = in_rect(x, y, lay.card_x, lay.card_y, lay.card_x + lay.card_w,
                           lay.card_y + lay.card_h);
    if (!in_card) {
        if (pm->armed >= 0) {
            pm->armed = -1;
            pm_render(pm);
        } else {
            pm_begin_close(pm);
        }
    }
}

void dc_powermenu_handle_motion(dc_powermenu *pm, double x, double y)
{
    if (!pm->visible || pm->closing)
        return;
    pm_layout lay = pm_get_layout((float)pm->logical_width, (float)pm->logical_height);
    int idx = pm_row_at(&lay, x, y);
    if (idx < 0 || idx == pm->selected)
        return;
    pm->selected = idx;
    if (pm->armed >= 0 && pm->armed != idx)
        pm->armed = -1;
    pm_render(pm);
}

void dc_powermenu_debug_fire(dc_powermenu *pm, int index)
{
    if (!pm->visible || pm->closing)
        return;
    pm_activate(pm, index); /* first activation: arms (or fires Lock directly) */
    if (pm->armed == index)
        pm_activate(pm, index); /* second activation on the still-armed row: confirms */
}

void dc_powermenu_destroy(dc_powermenu *pm)
{
    if (!pm)
        return;
    if (pm->visible)
        pm_teardown(pm);
    free(pm);
}
