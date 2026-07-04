#include "ui/popout.h"

#include "core/config.h"
#include "ui/bar/bar.h"

#include <stdbool.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Extra visual gap between the bar's rounded rect and the popout, on top of
 * dc_bar_window_height() (which already reaches exactly to the rect's
 * desktop-facing edge — see bar.h). docs/13-POPOUTS-SPEC.md sec.0: "offset =
 * barThickness + spacing + gap"; picked 8px total visual gap to match the
 * user's live DMS screenshots (popouts sit close, ~8-12px off the bar).
 * Only used when connected_frame is off -- see DC_SEAM_OVERLAP below. */
#define DC_POPOUT_BAR_GAP 8

/* connected_frame on: the popout's near edge tucks this many logical px
 * under the bar rect instead of sitting DC_POPOUT_BAR_GAP away from it, so
 * the two surfaces overlap by a hairline rather than showing a gap
 * (docs/27-CONNECTED-FRAME-PLAN.md G1). 1px (not 0) so fractional-scale
 * rounding never leaves a 1-physical-pixel seam of background visible
 * between the two independently-positioned layer surfaces. */
#define DC_SEAM_OVERLAP 1

/* Must match bar.c's DC_BAR_CORNER_RADIUS (static there, not exported). Only
 * used to clamp side_margin in connected mode so the connector fillets
 * (ui/connected.c) land directly under the bar's own rounded corner instead
 * of past it. */
#define DC_CONNECTED_CORNER_RADIUS 12

dc_popout_anchor dc_popout_bar_adjacent(const dc_config *cfg, dc_popout_align align,
                                        int32_t side_margin)
{
    dc_popout_anchor a = {0};
    const bool bottom = cfg->bar_position == DC_BAR_POSITION_BOTTOM;
    const int32_t window_height = (int32_t)dc_bar_window_height(cfg);
    int32_t bar_gap;

    if (cfg->connected_frame) {
        bar_gap = window_height - DC_SEAM_OVERLAP;
        if (bar_gap < 0)
            bar_gap = 0;
        const int32_t min_side_margin = cfg->bar_spacing + DC_CONNECTED_CORNER_RADIUS;
        if (side_margin < min_side_margin)
            side_margin = min_side_margin;
    } else {
        bar_gap = window_height + DC_POPOUT_BAR_GAP;
    }

    if (bottom) {
        a.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        a.margin_bottom = bar_gap;
        a.origin_y = 1.0f; /* animate from the bottom edge, growing toward the bar */
    } else {
        a.anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        a.margin_top = bar_gap;
        a.origin_y = 0.0f;
    }

    switch (align) {
    case DC_POPOUT_ALIGN_START:
        a.anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        a.margin_left = side_margin;
        a.origin_x = 0.0f;
        break;
    case DC_POPOUT_ALIGN_END:
        a.anchor |= ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        a.margin_right = side_margin;
        a.origin_x = 1.0f;
        break;
    case DC_POPOUT_ALIGN_CENTER:
    default:
        /* No horizontal anchor bits -> the compositor centers the surface
         * on that axis. */
        a.origin_x = 0.5f;
        break;
    }
    return a;
}

void dc_popout_chrome_pads(const dc_config *cfg, int *pad_near, int *pad_side, int *pad_far)
{
    if (cfg->connected_frame) {
        /* near=0: the card fill starts flush at the bar-facing edge (no
         * shadow room -- ui/connected.c scissors the shadow away from that
         * edge instead). side=12: room for the connector fillets, which are
         * DC_CONNECTED_CORNER_RADIUS wide (docs/27-CONNECTED-FRAME-PLAN.md
         * "surface widens 2*(12-6)"). far=6: unchanged drop-shadow room on
         * the side away from the bar. */
        if (pad_near)
            *pad_near = 0;
        if (pad_side)
            *pad_side = 12;
        if (pad_far)
            *pad_far = 6;
    } else {
        /* Today's floating chrome: 6px of shadow room on all four sides. */
        if (pad_near)
            *pad_near = 6;
        if (pad_side)
            *pad_side = 6;
        if (pad_far)
            *pad_far = 6;
    }
}
