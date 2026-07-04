#include "ui/battery_popout.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/battery.h"
#include "services/power.h"
#include "theme/theme.h"
#include "ui/connected.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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
/* +26px over the original 260 to fit the raw-profile-name caption line under
 * the power-profile segments (see bp_get_layout's caption_y/caption_h). */
#define DC_BP_HEIGHT 286
#define DC_SCALE_BASE 120
/* Inset from the screen's right edge when bar-adjacent. Battery sits just
 * left of controlCenterButton in the bar's right cluster (bar.c's widget
 * table), but dc_popout_bar_adjacent() only anchors to a screen edge, not a
 * specific widget -- same approximation controlcenter.c makes for the CC
 * pill itself ("opens near the bar's right cluster", not pixel-exact). */
#define DC_BP_SIDE_MARGIN 12

/* Logical surface width. DC_BP_WIDTH already bakes in the floating chrome's
 * flat 6px pad on every side; connected_frame widens the lateral (side) pad
 * to 12 for the connector fillets (dc_popout_chrome_pads()), so the surface
 * needs 2*(pad_side-6) more logical px to keep the card CONTENT rect --
 * inset by pad_side + margin, see bp_get_layout() -- exactly where it sits
 * when floating (mirrors controlcenter.c's cc_surface_width()).
 * connected_frame off: pad_side==6, so this is just DC_BP_WIDTH, unchanged. */
static int bp_surface_width(void)
{
    int pad_side = 6;
    dc_popout_chrome_pads(dc_config_current, NULL, &pad_side, NULL);
    return DC_BP_WIDTH + 2 * (pad_side - 6);
}

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

/* --- power profiles: src/services/power.c does the real work (backend
 * detection across power-profiles-daemon / tuned D-Bus / tuned-adm CLI,
 * caching, mode<->profile mapping). This file just renders the 3-mode
 * segmented control and the raw-profile caption. --------------------------- */

static const char *const power_mode_labels[3] = {"Power Saver", "Balanced", "Performance"};

/* Exact DMS slug for each mode -- used to decide whether the raw active
 * profile needs a caption (docs task: "show the raw tuned profile name as a
 * small caption if it doesn't exactly match a mode"). */
static const char *const power_mode_slugs[3] = {"power-saver", "balanced", "performance"};

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
    float seg_x[3], seg_w[3], seg_gap; /* content-sized, not equal thirds -- see bp_get_layout() */

    float caption_y, caption_h; /* raw backend profile name, e.g. "throughput-performance" */
} bp_layout;

/* Power-profile chip sizing (docs/13-POPOUTS-SPEC.md sec.2 fix: the old
 * equal-thirds split gave each chip (iw - 2*gap)/3 == 97px with zero slack,
 * so the active chip's check-mark + label overflowed its pill and clipped
 * at the card edge). DMS sizes each chip to its own label instead of
 * splitting the row evenly -- match that: measure each label once and give
 * it just enough padding, so only the (wider) active chip grows. */
#define DC_BP_SEG_FONT 13.0f
#define DC_BP_SEG_PAD 10.0f
#define DC_BP_SEG_MIN_W 66.0f
#define DC_BP_SEG_ICON_W 14.0f
#define DC_BP_SEG_ICON_GAP 5.0f

/* power_mode_labels[] are fixed, compile-time-known English strings and the
 * card is a fixed width, so their glyph widths never change at runtime --
 * measure once (first bp_render(), the only call site with a live nvg
 * frame) and cache. bp_get_layout() also runs from handle_click() for hit-
 * testing, with no frame active, so it can only ever read this cache, never
 * measure. */
static float g_seg_label_w[3] = {-1.0f, -1.0f, -1.0f};

static void bp_ensure_label_widths(dc_render *r)
{
    if (g_seg_label_w[0] >= 0.0f)
        return;
    NVGcontext *vg = r->vg;
    nvgSave(vg);
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, DC_BP_SEG_FONT);
    for (int i = 0; i < 3; i++) {
        float bounds[4];
        nvgTextBounds(vg, 0.0f, 0.0f, power_mode_labels[i], NULL, bounds);
        g_seg_label_w[i] = bounds[2] - bounds[0];
    }
    nvgRestore(vg);
}

/* `icon_mode` is the index (0-2) of the segment that will draw the
 * check-mark this frame (docs/13-POPOUTS-SPEC.md sec.2's "active, enabled"
 * segment), or -1 when none will (daemon unavailable). Only that segment's
 * width reserves room for the icon, matching draw_profile_segment()'s own
 * `active && !dimmed` gate for actually drawing it. */
static bp_layout bp_get_layout(float w, int icon_mode)
{
    /* Card-fill padding (docs/27-CONNECTED-FRAME-PLAN.md T5): floating
     * chrome reserves a flat 6px of shadow room on all four sides;
     * connected chrome widens the lateral (side) pad to 12 for the
     * connector fillets and drops the bar-facing (near) pad to 0 -- see
     * dc_popout_chrome_pads(). Which physical edge is "near" swaps with
     * bar_position; self-contained the same way controlcenter.c's
     * cc_get_layout() reads dc_config_current directly. */
    int pad_near, pad_side, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, &pad_side, &pad_far);
    const bool bottom_bar = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    const float pad_top = bottom_bar ? (float)pad_far : (float)pad_near;
    const float pad_side_f = (float)pad_side;
    const float margin = 20.0f; /* content inset from the card edge */
    const float gap = 16.0f;

    bp_layout l = {0};
    l.ix = pad_side_f + margin;
    l.iw = w - 2.0f * l.ix;

    l.header_y = pad_top + margin;
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
    l.profiles_h = 40.0f;
    l.seg_gap = 6.0f;

    float chip_w[3];
    float total = 2.0f * l.seg_gap;
    for (int i = 0; i < 3; i++) {
        float lw = g_seg_label_w[i] >= 0.0f ? g_seg_label_w[i] : 60.0f; /* pre-cache fallback */
        float content = lw + (i == icon_mode ? DC_BP_SEG_ICON_W + DC_BP_SEG_ICON_GAP : 0.0f);
        chip_w[i] = content + 2.0f * DC_BP_SEG_PAD;
        if (chip_w[i] < DC_BP_SEG_MIN_W)
            chip_w[i] = DC_BP_SEG_MIN_W;
        total += chip_w[i];
    }

    /* Centered as a group, like DMS's Row; clamp to the card's left inset
     * in the (should-never-happen, but the 3 labels are fixed English
     * strings so it's cheap insurance) case the row doesn't fit at all. */
    float start_x = l.ix + (l.iw - total) / 2.0f;
    if (start_x < l.ix)
        start_x = l.ix;
    float x = start_x;
    for (int i = 0; i < 3; i++) {
        l.seg_x[i] = x;
        l.seg_w[i] = chip_w[i];
        x += chip_w[i] + l.seg_gap;
    }

    l.caption_y = l.profiles_y + l.profiles_h + 8.0f;
    l.caption_h = 18.0f;

    return l;
}

/* Material Symbols glyph for the header battery icon -- same tiering as
 * bar.c's static battery_icon_codepoint() (that one isn't exported; small
 * duplication is cheaper than widening battery.h's touch-scope for this). */
static int bp_battery_icon(bool charging, bool full, int percent)
{
    if (full)
        return DC_ICON_BATTERY_CHARGING_FULL;
    if (charging) {
        if (percent >= 90)
            return DC_ICON_BATTERY_CHARGING_FULL;
        if (percent >= 80)
            return DC_ICON_BATTERY_CHARGING_90;
        if (percent >= 60)
            return DC_ICON_BATTERY_CHARGING_80;
        if (percent >= 50)
            return DC_ICON_BATTERY_CHARGING_60;
        if (percent >= 30)
            return DC_ICON_BATTERY_CHARGING_50;
        if (percent >= 20)
            return DC_ICON_BATTERY_CHARGING_30;
        return DC_ICON_BATTERY_CHARGING_20;
    }
    if (percent >= 95)
        return DC_ICON_BATTERY_FULL;
    if (percent >= 85)
        return DC_ICON_BATTERY_6_BAR;
    if (percent >= 70)
        return DC_ICON_BATTERY_5_BAR;
    if (percent >= 55)
        return DC_ICON_BATTERY_4_BAR;
    if (percent >= 40)
        return DC_ICON_BATTERY_3_BAR;
    if (percent >= 25)
        return DC_ICON_BATTERY_2_BAR;
    if (percent >= 10)
        return DC_ICON_BATTERY_1_BAR;
    return DC_ICON_BATTERY_ALERT;
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

/* The segment index that gets the check-mark this frame, or -1 when none
 * does -- shared by bp_render() (draw) and handle_click() (hit-test) so the
 * layout they each compute from it always agrees with what's on screen. */
static int bp_icon_mode(bool pw_avail, const dc_power_info *pw)
{
    if (!pw_avail)
        return -1;
    if (pw->active_mode == DC_POWER_MODE_PERFORMANCE && !pw->has_performance_mode)
        return -1;
    return (int)pw->active_mode;
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
    nvgFontSize(vg, DC_BP_SEG_FONT);
    if (active && !dimmed) {
        /* Check icon + label, centered as a group (CompoundPill-ish). Icon
         * size/gap must match bp_get_layout()'s reservation exactly, or the
         * chip is sized for one width and drawn at another. */
        float bounds[4];
        nvgTextBounds(vg, 0.0f, 0.0f, label, NULL, bounds);
        float text_w = bounds[2] - bounds[0];
        float icon_w = DC_BP_SEG_ICON_W;
        float total = icon_w + DC_BP_SEG_ICON_GAP + text_w;
        float start_x = cx - total / 2.0f;
        dc_render_icon(r, DC_ICON_DONE, start_x, cy, icon_w, text_fg,
                       NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(text_fg));
        nvgText(vg, start_x + icon_w + DC_BP_SEG_ICON_GAP, cy, label, NULL);
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
    const bool bottom_bar = dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
    int pad_near, pad_side, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, &pad_side, &pad_far);
    const float pad_top = bottom_bar ? (float)pad_far : (float)pad_near;
    const float pad_bottom = bottom_bar ? (float)pad_near : (float)pad_far;
    const float pad_side_f = (float)pad_side;

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
    float ox = pad_side_f + (w - 2.0f * pad_side_f) * bp->anim_ox;
    float oy = pad_top + (h - pad_top - pad_bottom) * bp->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Card chrome: shadow + fill + outline, floating or stitched into the
     * bar depending on connected_frame -- see ui/connected.h. Byte-identical
     * to the old inline floating-chrome block when the toggle is off. */
    dc_connected_card_chrome(vg, bp->render, w, h, bottom_bar);

    bp_ensure_label_widths(bp->render);
    dc_power_info pw = {0};
    bool pw_avail = dc_power_read(&pw);
    bp_layout l = bp_get_layout(w, bp_icon_mode(pw_avail, &pw));

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

    /* --- Power profile segmented control --------------------------------- */
    for (int i = 0; i < 3; i++) {
        bool seg_dimmed =
            !pw_avail || (i == DC_POWER_MODE_PERFORMANCE && !pw.has_performance_mode);
        draw_profile_segment(bp->render, l.seg_x[i], l.profiles_y, l.seg_w[i], l.profiles_h,
                             power_mode_labels[i], pw.active_mode == i, seg_dimmed);
    }

    /* Raw backend profile name as a caption, only when it doesn't exactly
     * match one of the 3 mode slugs -- tuned's "throughput-performance" maps
     * onto Performance but isn't literally "performance". */
    if (pw_avail && pw.active_profile[0] &&
        !(pw.active_mode != DC_POWER_MODE_UNKNOWN &&
          strcmp(pw.active_profile, power_mode_slugs[pw.active_mode]) == 0)) {
        const char *prefix = (pw.backend == DC_POWER_BACKEND_TUNED_DBUS ||
                              pw.backend == DC_POWER_BACKEND_TUNED_CLI)
                                 ? "tuned: "
                                 : "";
        char caption[96];
        snprintf(caption, sizeof(caption), "%s%s", prefix, pw.active_profile);
        nvgFontFaceId(vg, bp->render->font_ui);
        nvgFontSize(vg, 12.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_variant_text, 200));
        nvgText(vg, l.ix + l.iw / 2.0f, l.caption_y + l.caption_h / 2.0f, caption, NULL);
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
    bp->logical_width = width > 0 ? (int)width : bp_surface_width();
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
    bp->logical_width = bp_surface_width();
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
    bp->logical_width = bp_surface_width();
    zwlr_layer_surface_v1_set_size(bp->layer_surface, (uint32_t)bp->logical_width, DC_BP_HEIGHT);
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

    dc_power_info pw = {0};
    bool pw_avail = dc_power_read(&pw);
    bp_layout l = bp_get_layout((float)bp->logical_width, bp_icon_mode(pw_avail, &pw));

    /* Close button. */
    double dx = x - (double)l.close_cx;
    double dy = y - (double)l.close_cy;
    if (dx * dx + dy * dy <= (double)(l.close_r * l.close_r)) {
        bp_begin_close(bp);
        return;
    }

    /* Power profile segments. */
    if (y >= (double)l.profiles_y && y <= (double)(l.profiles_y + l.profiles_h)) {
        if (!pw_avail) {
            /* No backend (power-profiles-daemon, tuned D-Bus, or tuned-adm)
             * found -- row stays dimmed and non-interactive. */
            dc_debug("battery popout: power profiles unavailable");
            return;
        }
        for (int i = 0; i < 3; i++) {
            if (x < (double)l.seg_x[i] || x > (double)(l.seg_x[i] + l.seg_w[i]))
                continue;
            if (i == DC_POWER_MODE_PERFORMANCE && !pw.has_performance_mode)
                return; /* dimmed segment, not selectable */
            dc_power_set_mode((dc_power_mode)i);
            bp_render(bp);
            return;
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
