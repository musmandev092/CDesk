#include "ui/popout.h"

#include "core/config.h"
#include "ui/bar/bar.h"

#include <stdbool.h>

#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Extra visual gap between the bar's rounded rect and the popout, on top of
 * dc_bar_window_height() (which already reaches exactly to the rect's
 * desktop-facing edge — see bar.h). docs/13-POPOUTS-SPEC.md sec.0: "offset =
 * barThickness + spacing + gap"; picked 8px total visual gap to match the
 * user's live DMS screenshots (popouts sit close, ~8-12px off the bar). */
#define DC_POPOUT_BAR_GAP 8

dc_popout_anchor dc_popout_bar_adjacent(const dc_config *cfg, dc_popout_align align,
                                        int32_t side_margin)
{
    dc_popout_anchor a = {0};
    const bool bottom = cfg->bar_position == DC_BAR_POSITION_BOTTOM;
    const int32_t bar_gap = (int32_t)dc_bar_window_height(cfg) + DC_POPOUT_BAR_GAP;

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
