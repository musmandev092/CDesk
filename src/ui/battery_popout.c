#include "ui/battery_popout.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/battery.h"
#include "theme/theme.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Size picked to match the task spec + the user's live DMS reference
 * screenshot (~/Pictures/Screenshots/Screenshot from 2026-07-02 14-17-35.png):
 * a small card, not a full popout -- header row (icon/percent/status/close),
 * two stat cards (Health/Capacity), and a 3-way power-profile segmented
 * control (docs/13-POPOUTS-SPEC.md sec.2). */
#define DC_BP_WIDTH 360
#define DC_BP_HEIGHT 260
#define DC_SCALE_BASE 120
/* Inset from the screen's right edge when bar-adjacent. Battery sits just
 * left of controlCenterButton in the bar's right cluster (bar.c's widget
 * table), but dc_popout_bar_adjacent() only anchors to a screen edge, not a
 * specific widget -- same approximation controlcenter.c makes for the CC
 * pill itself ("opens near the bar's right cluster", not pixel-exact). */
#define DC_BP_SIDE_MARGIN 12

struct dc_battery_popout {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width;
    int logical_height;
    int scale120;
    int phys_width;
    int phys_height;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;

    float anim_ox, anim_oy;

    bool visible;
    bool configured;
    bool egl_ready;
};

static void bp_render(dc_battery_popout *bp);
static void bp_teardown(dc_battery_popout *bp);

static void bp_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_battery_popout *bp = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    bp->frame_cb = NULL;
    if (!bp->visible)
        return;
    if (dc_anim_active(&bp->anim))
        bp_render(bp);
    else if (bp->closing)
        bp_teardown(bp);
}

static const struct wl_callback_listener bp_frame_listener = {.done = bp_frame_done};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

/* Run a shell command detached (children auto-reaped via SIG_IGN on SIGCHLD,
 * set in main.c) -- same pattern as controlcenter.c's run_detached(). */
static void run_detached(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* --- power-profiles-daemon, via `powerprofilesctl` (docs/13-POPOUTS-SPEC.md
 * sec.2: "power-profiles-daemon over D-Bus (or powerprofilesctl)" -- the CLI
 * avoids a direct sd-bus dependency for a single 3-state toggle). ---------- */

typedef enum {
    DC_PP_POWER_SAVER = 0,
    DC_PP_BALANCED = 1,
    DC_PP_PERFORMANCE = 2,
} dc_power_profile;

static const char *const power_profile_names[3] = {"power-saver", "balanced", "performance"};
static const char *const power_profile_labels[3] = {"Power Saver", "Balanced", "Performance"};

/* `command -v powerprofilesctl` once per process -- cheap and the binary's
 * presence doesn't change at runtime. */
static bool power_profiles_available(void)
{
    static int cached = -1; /* -1 unknown, 0 no, 1 yes */
    if (cached < 0)
        cached = (system("command -v powerprofilesctl >/dev/null 2>&1") == 0) ? 1 : 0;
    return cached == 1;
}

/* Currently-active profile index, or -1 if unavailable/unknown. Cached
 * per-second like controlcenter.c's audio_source_read(), since this can be
 * queried once per render frame during the entrance animation. */
static int active_power_profile(void)
{
    static int cache = -1;
    static time_t cache_time;

    if (!power_profiles_available())
        return -1;

    time_t now = time(NULL);
    if (cache_time == now)
        return cache;
    cache_time = now;
    cache = -1;

    FILE *pipe = popen("powerprofilesctl get 2>/dev/null", "r");
    if (pipe) {
        char line[64];
        if (fgets(line, sizeof(line), pipe)) {
            line[strcspn(line, "\n")] = '\0';
            for (int i = 0; i < 3; i++) {
                if (strcmp(line, power_profile_names[i]) == 0) {
                    cache = i;
                    break;
                }
            }
        }
        pclose(pipe);
    }
    return cache;
}

static void set_power_profile(dc_power_profile p)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "powerprofilesctl set %s", power_profile_names[p]);
    run_detached(cmd);
}

/* --- layout: shared by bp_render (draw) and handle_click (hit-test) ------ */

typedef struct {
    float ix, iw;

    float header_y, header_h, header_cy;
    float icon_x;      /* icon left edge */
    float percent_x;   /* percent text left edge */
    float status_x;    /* status text left edge */
    float close_cx, close_cy, close_r;

    float stats_y, stat_h, stat_w, stat_gap;
    float stat_x[2];

    float profiles_y, profiles_h;
    float seg_x[3], seg_w, seg_gap;
} bp_layout;

static bp_layout bp_get_layout(float w)
{
    const float pad = 6.0f;    /* room for the drop shadow */
    const float margin = 20.0f; /* content inset from the card edge */
    const float gap = 16.0f;

    bp_layout l = {0};
    l.ix = pad + margin;
    l.iw = w - 2.0f * l.ix;

    l.header_y = pad + margin;
    l.header_h = 48.0f;
    l.header_cy = l.header_y + l.header_h / 2.0f;
    l.icon_x = l.ix;
    l.percent_x = l.ix + 38.0f;
    l.status_x = l.percent_x + 66.0f; /* room for "100%" at the percent font size */
    l.close_r = 14.0f;
    l.close_cx = l.ix + l.iw - l.close_r;
    l.close_cy = l.header_cy;

    l.stats_y = l.header_y + l.header_h + gap;
    l.stat_h = 80.0f;
    l.stat_gap = gap;
    l.stat_w = (l.iw - l.stat_gap) / 2.0f;
    l.stat_x[0] = l.ix;
    l.stat_x[1] = l.ix + l.stat_w + l.stat_gap;

    l.profiles_y = l.stats_y + l.stat_h + gap;
    l.profiles_h = 48.0f;
    l.seg_gap = 8.0f;
    l.seg_w = (l.iw - 2.0f * l.seg_gap) / 3.0f;
    for (int i = 0; i < 3; i++)
        l.seg_x[i] = l.ix + (float)i * (l.seg_w + l.seg_gap);

    return l;
}

/* Material Symbols glyph for the header battery icon -- same tiering as
 * bar.c's static battery_icon_codepoint() (that one isn't exported; small
 * duplication is cheaper than widening battery.h's touch-scope for this). */
static int bp_battery_icon(bool charging, bool full, int percent)
{
    if (charging || full)
        return DC_ICON_BATTERY_CHARGING_FULL;
    if (percent <= 10)
        return DC_ICON_BATTERY_ALERT;
    if (percent <= 25)
        return DC_ICON_BATTERY_0_BAR;
    return DC_ICON_BATTERY_FULL;
}

/* Health card value color, tiered by degradation (docs/13-POPOUTS-SPEC.md
 * sec.2). The middle tier's RGB was sampled directly from the reference
 * screenshot's "64%" glyph (a muted coral, not the saturated theme `error`
 * red) -- no existing theme token covers that tone, so it's hardcoded here
 * rather than misusing `error`/`warning` for a color that doesn't match. */
static dc_color bp_health_color(const dc_theme *t, int health_percent)
{
    if (health_percent < 0)
        return t->surface_text;
    if (health_percent >= 80)
        return t->primary;
    if (health_percent >= 50) {
        dc_color coral = {242, 184, 181, 255};
        return coral;
    }
    return t->error;
}

static void recompute_physical(dc_battery_popout *bp)
{
    bp->phys_width = (bp->logical_width * bp->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    bp->phys_height = (bp->logical_height * bp->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void draw_stat_card(dc_render *r, float x, float y, float w, float h, const char *label,
                           const char *value, dc_color value_color)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    const float cx = x + w / 2.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    nvgFontFaceId(vg, r->font_ui);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgText(vg, cx, y + 24.0f, label, NULL);

    nvgFontSize(vg, 22.0f);
    nvgFillColor(vg, tc(value_color));
    nvgText(vg, cx, y + 54.0f, value, NULL);
}

/* One Power Saver/Balanced/Performance segment. `dimmed` = the daemon isn't
 * available at all (docs/13-POPOUTS-SPEC.md sec.2: "absent -> dimmed row"). */
static void draw_profile_segment(dc_render *r, float x, float y, float w, float h,
                                 const char *label, bool active, bool dimmed)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    const float cx = x + w / 2.0f;
    const float cy = y + h / 2.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 10.0f);
    nvgFillColor(vg, active && !dimmed ? tc(t->primary) : tc(t->surface_container_high));
    nvgFill(vg);

    int text_alpha = dimmed ? 90 : 255;
    dc_color text_fg = (active && !dimmed) ? t->primary_text : t->surface_text;

    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 13.0f);
    if (active && !dimmed) {
        /* Check icon + label, centered as a group (CompoundPill-ish). */
        float bounds[4];
        nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
        float text_w = bounds[2] - bounds[0];
        float icon_w = 16.0f;
        float total = icon_w + 4.0f + text_w;
        float start_x = cx - total / 2.0f;
        dc_render_icon(r, DC_ICON_DONE, start_x, cy, icon_w, text_fg,
                       NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(text_fg));
        nvgText(vg, start_x + icon_w + 4.0f, cy, label, NULL);
    } else {
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(text_fg, text_alpha));
        nvgText(vg, cx, cy, label, NULL);
    }
}

static void bp_render(dc_battery_popout *bp)
{
    if (!bp->configured || bp->phys_width <= 0)
        return;

    if (!bp->egl_ready) {
        if (!dc_egl_window_init(&bp->egl_window, bp->egl, bp->surface, bp->phys_width,
                                bp->phys_height))
            return;
        bp->egl_ready = true;
    } else {
        dc_egl_window_resize(&bp->egl_window, bp->phys_width, bp->phys_height);
    }

    if (!dc_egl_make_current(bp->egl, &bp->egl_window))
        return;
    if (!dc_render_ensure(bp->render))
        return;

    if (bp->viewport)
        wp_viewport_set_destination(bp->viewport, bp->logical_width, bp->logical_height);

    NVGcontext *vg = bp->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = bp->logical_width;
    const float h = bp->logical_height;
    const float pad = 6.0f;

    glViewport(0, 0, bp->phys_width, bp->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)bp->scale120 / DC_SCALE_BASE);

    /* Entrance/exit: fade + scale from the bar-facing edge, same as
     * controlcenter.c's cc_render(). */
    float p = dc_anim_progress(&bp->anim);
    if (bp->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.94f + 0.06f * p;
    float ox = pad + (w - 2.0f * pad) * bp->anim_ox;
    float oy = pad + (h - 2.0f * pad) * bp->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Soft drop shadow. */
    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 12.0f, 18.0f,
                                     nvgRGBA(0, 0, 0, 90), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    /* Card: opaque surfaceContainer (docs/13-POPOUTS-SPEC.md sec.0: user
     * popupTransparency=1 -> opaque). */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgFillColor(vg, nvgRGBA(t->surface_container.r, t->surface_container.g, t->surface_container.b,
                             255));
    nvgFill(vg);
    nvgStrokeColor(vg, nvgRGBA(t->outline.r, t->outline.g, t->outline.b, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    bp_layout l = bp_get_layout(w);

    dc_battery_info bat;
    bool have = dc_battery_read(&bat) && bat.present;

    /* --- Header: icon, big percent, status, close --------------------- */
    int icon_cp = have ? bp_battery_icon(bat.charging, bat.full, bat.percent) : DC_ICON_BATTERY_ALERT;
    dc_color icon_color = (have && (bat.charging || bat.full)) ? t->primary : t->surface_text;
    dc_render_icon(bp->render, icon_cp, l.icon_x, l.header_cy, 28.0f, icon_color,
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    char percent_buf[8];
    if (have)
        snprintf(percent_buf, sizeof(percent_buf), "%d%%", bat.percent);
    else
        snprintf(percent_buf, sizeof(percent_buf), "--");
    nvgFontFaceId(vg, bp->render->font_ui);
    nvgFontSize(vg, 26.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, l.percent_x, l.header_cy, percent_buf, NULL);

    const char *status =
        !have ? "No Battery" : (bat.full ? "Fully Charged" : (bat.charging ? "Charging" : "Discharging"));
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, l.status_x, l.header_cy, status, NULL);

    dc_render_icon(bp->render, DC_ICON_CLOSE, l.close_cx, l.close_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    /* --- Stat cards: Health / Capacity --------------------------------- */
    char health_buf[16];
    if (have && bat.health_percent >= 0)
        snprintf(health_buf, sizeof(health_buf), "%d%%", bat.health_percent);
    else
        snprintf(health_buf, sizeof(health_buf), "--");
    dc_color health_color = bp_health_color(t, bat.health_percent);
    draw_stat_card(bp->render, l.stat_x[0], l.stats_y, l.stat_w, l.stat_h, "Health", health_buf,
                  health_color);

    char capacity_buf[16];
    if (have && bat.energy_full_wh >= 0.0)
        snprintf(capacity_buf, sizeof(capacity_buf), "%.1f Wh", bat.energy_full_wh);
    else
        snprintf(capacity_buf, sizeof(capacity_buf), "--");
    draw_stat_card(bp->render, l.stat_x[1], l.stats_y, l.stat_w, l.stat_h, "Capacity", capacity_buf,
                  t->surface_text);

    /* --- Power profile segmented control ------------------------------- */
    bool pp_avail = power_profiles_available();
    int active = pp_avail ? active_power_profile() : -1;
    for (int i = 0; i < 3; i++) {
        draw_profile_segment(bp->render, l.seg_x[i], l.profiles_y, l.seg_w, l.profiles_h,
                             power_profile_labels[i], active == i, !pp_avail);
    }

    nvgEndFrame(vg);

    if ((dc_anim_active(&bp->anim) || bp->closing) && !bp->frame_cb) {
        bp->frame_cb = wl_surface_frame(bp->surface);
        wl_callback_add_listener(bp->frame_cb, &bp_frame_listener, bp);
    }
    dc_egl_swap(bp->egl, &bp->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_battery_popout *bp = data;
    DC_UNUSED(fs);
    bp->scale120 = (int)scale;
    recompute_physical(bp);
    bp_render(bp);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_battery_popout *bp = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    bp->logical_width = width > 0 ? (int)width : DC_BP_WIDTH;
    bp->logical_height = height > 0 ? (int)height : DC_BP_HEIGHT;
    bp->configured = true;
    recompute_physical(bp);
    bp_render(bp);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_battery_popout *bp = data;
    DC_UNUSED(surface);
    bp->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_battery_popout *dc_battery_popout_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_battery_popout *bp = calloc(1, sizeof(*bp));
    bp->wl = wl;
    bp->egl = egl;
    bp->render = render;
    bp->logical_width = DC_BP_WIDTH;
    bp->logical_height = DC_BP_HEIGHT;
    bp->scale120 = DC_SCALE_BASE;
    return bp;
}

static void bp_show(dc_battery_popout *bp, dc_output *output)
{
    bp->output = output;
    bp->configured = false;
    bp->egl_ready = false;
    bp->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    dc_anim_start(&bp->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    bp->surface = wl_compositor_create_surface(bp->wl->compositor);
    if (bp->wl->fractional_scale_mgr) {
        bp->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            bp->wl->fractional_scale_mgr, bp->surface);
        wp_fractional_scale_v1_add_listener(bp->fractional_scale, &fractional_scale_listener, bp);
    }
    if (bp->wl->viewporter)
        bp->viewport = wp_viewporter_get_viewport(bp->wl->viewporter, bp->surface);

    bp->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        bp->wl->layer_shell, bp->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:battery");

    /* Bar-adjacent, right-aligned (docs/13-POPOUTS-SPEC.md sec.0/2). */
    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_END, DC_BP_SIDE_MARGIN);
    bp->anim_ox = pa.origin_x;
    bp->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(bp->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(bp->layer_surface, DC_BP_WIDTH, DC_BP_HEIGHT);
    zwlr_layer_surface_v1_set_margin(bp->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_exclusive_zone(bp->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(bp->layer_surface, &layer_surface_listener, bp);

    wl_surface_commit(bp->surface);
    bp->visible = true;
    bp->closing = false;
    dc_debug("battery popout shown");
}

static void bp_teardown(dc_battery_popout *bp)
{
    if (bp->frame_cb) {
        wl_callback_destroy(bp->frame_cb);
        bp->frame_cb = NULL;
    }
    if (bp->egl_ready)
        dc_egl_window_finish(&bp->egl_window, bp->egl);
    if (bp->viewport)
        wp_viewport_destroy(bp->viewport);
    if (bp->fractional_scale)
        wp_fractional_scale_v1_destroy(bp->fractional_scale);
    if (bp->layer_surface)
        zwlr_layer_surface_v1_destroy(bp->layer_surface);
    if (bp->surface)
        wl_surface_destroy(bp->surface);
    bp->egl_ready = false;
    bp->configured = false;
    bp->viewport = NULL;
    bp->fractional_scale = NULL;
    bp->layer_surface = NULL;
    bp->surface = NULL;
    bp->visible = false;
    bp->closing = false;
    dc_debug("battery popout hidden");
}

static void bp_begin_close(dc_battery_popout *bp)
{
    if (!bp->visible || bp->closing)
        return;
    dc_anim_start(&bp->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    bp->closing = true;
    if (!dc_anim_active(&bp->anim)) {
        bp_teardown(bp);
        return;
    }
    bp_render(bp);
}

void dc_battery_popout_toggle(dc_battery_popout *bp, dc_output *output)
{
    if (bp->visible)
        bp_begin_close(bp);
    else
        bp_show(bp, output);
}

bool dc_battery_popout_visible(dc_battery_popout *bp)
{
    return bp->visible;
}

void dc_battery_popout_hide(dc_battery_popout *bp)
{
    bp_begin_close(bp);
}

struct wl_surface *dc_battery_popout_surface(dc_battery_popout *bp)
{
    return bp->surface;
}

void dc_battery_popout_handle_click(dc_battery_popout *bp, double x, double y)
{
    if (!bp->visible || bp->closing)
        return;

    bp_layout l = bp_get_layout((float)bp->logical_width);

    /* Close button. */
    double dx = x - (double)l.close_cx;
    double dy = y - (double)l.close_cy;
    if (dx * dx + dy * dy <= (double)(l.close_r * l.close_r)) {
        bp_begin_close(bp);
        return;
    }

    /* Power profile segments. */
    if (y >= (double)l.profiles_y && y <= (double)(l.profiles_y + l.profiles_h)) {
        if (!power_profiles_available()) {
            /* TODO(P4-power-profiles): no power-profiles-daemon/powerprofilesctl
             * found -- row stays dimmed and non-interactive until a fallback
             * (direct sd-bus call, or a different backend) is added. */
            dc_debug("battery popout: power profiles unavailable (TODO)");
            return;
        }
        for (int i = 0; i < 3; i++) {
            if (x >= (double)l.seg_x[i] && x <= (double)(l.seg_x[i] + l.seg_w)) {
                set_power_profile((dc_power_profile)i);
                bp_render(bp);
                return;
            }
        }
    }
}

void dc_battery_popout_destroy(dc_battery_popout *bp)
{
    if (!bp)
        return;
    if (bp->visible)
        bp_teardown(bp);
    free(bp);
}
