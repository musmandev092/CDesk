/* popout.h — bar-position-aware popout anchoring, shared by every panel that
 * should open adjacent to the bar (docs/13-POPOUTS-SPEC.md sec.0): control
 * center, notification center, clipboard picker, settings, launcher.
 *
 * The bar can live on the top or bottom screen edge (dc_config.bar_position).
 * Before this helper, every popout hardcoded ANCHOR_TOP|ANCHOR_RIGHT (or no
 * anchor at all, i.e. screen-centered) with fixed margins — correct only for
 * a top bar. dc_popout_bar_adjacent() computes the layer-shell anchor bits,
 * margins, and an entrance-animation origin so a popout always opens flush
 * against whichever edge the bar is actually on, regardless of config.
 */
#ifndef DC_UI_POPOUT_H
#define DC_UI_POPOUT_H

#include <stdint.h>

struct dc_config;

/* Alignment along the bar's own axis (screen-horizontal — the bar itself is
 * always full-width). Independent of bar_position (top/bottom). */
typedef enum {
    DC_POPOUT_ALIGN_START,  /* flush with the screen's left edge */
    DC_POPOUT_ALIGN_CENTER, /* horizontally centered */
    DC_POPOUT_ALIGN_END,    /* flush with the screen's right edge */
} dc_popout_align;

/* Layer-shell anchor + margins for a popout that opens directly adjacent to
 * the bar, plus a normalized entrance/exit scale-and-fade origin (panels
 * used to hardcode this per hand-picked corner; now it always points at the
 * bar-facing edge). `origin_x`/`origin_y` are fractions of the popout's own
 * logical size (0 = left/top edge, 0.5 = center, 1 = right/bottom edge):
 * apply as `ox = pad + (w - 2*pad) * origin_x` (and similarly for y) as the
 * pivot for a translate/scale/translate-back entrance animation. */
typedef struct {
    uint32_t anchor; /* zwlr_layer_surface_v1_anchor bitmask */
    int32_t margin_top;
    int32_t margin_right;
    int32_t margin_bottom;
    int32_t margin_left;
    float origin_x;
    float origin_y;
} dc_popout_anchor;

/* Compute anchor/margins/anim-origin for a popout that sits `side_margin`
 * logical px in from the left/right screen edge (used only for
 * START/END align) and opens with an ~8px visual gap above the bar's
 * rounded rect (bar at bottom) or below it (bar at top) — see
 * dc_bar_window_height(), which already accounts for the bar's own
 * outer-edge spacing gap.
 *
 * When cfg->connected_frame is on, the gap collapses to a 1px seam overlap
 * instead (docs/27-CONNECTED-FRAME-PLAN.md G1) and `side_margin` is clamped
 * upward so the connector fillets a converted panel draws (ui/connected.c)
 * land directly under the bar's own rounded corner. When it's off, behavior
 * is byte-identical to before this option existed. */
dc_popout_anchor dc_popout_bar_adjacent(const struct dc_config *cfg, dc_popout_align align,
                                        int32_t side_margin);

/* Card-fill padding a bar-adjacent popout should reserve on its near side
 * (the bar-facing edge), its two lateral sides, and its far side, instead of
 * every panel hardcoding a flat 6px on all four (the assumption baked into
 * today's floating chrome). Any output pointer may be NULL.
 *
 * connected_frame off: near=side=far=6 (today's floating chrome, unchanged).
 * connected_frame on: near=0 (card fill is flush with the bar, no shadow
 * room needed there), side=12 (room for the connector fillets), far=6
 * (unchanged). Pass these to dc_connected_card_chrome() call sites (T3+)
 * instead of a hardcoded `pad = 6.0f`. */
void dc_popout_chrome_pads(const struct dc_config *cfg, int *pad_near, int *pad_side,
                           int *pad_far);

#endif /* DC_UI_POPOUT_H */
