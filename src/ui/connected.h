/* connected.h — shared "connected-frame" chrome for popout panels
 * (docs/27-CONNECTED-FRAME-PLAN.md). When dc_config_current->connected_frame
 * is on, a bar-adjacent popout should read as a single continuous surface
 * that emerges from the bar rather than a separate floating card: square
 * near-edge corners flush with the bar, rounded far corners, two concave
 * "connector" fillets bridging the near corners into the bar's own rounded
 * corners, a shadow that never darkens the seam, and an outline open on the
 * near (bar-facing) side.
 *
 * dc_connected_card_chrome() is the single call site a panel uses regardless
 * of the toggle: it internally branches on connected_frame and reproduces
 * today's floating chrome (shadow + dc_material_bg_fill_card(12) + 1px
 * outline, pad 6 all around) byte-for-byte when the toggle is off.
 *
 * No panel calls this yet (T2 only provides the helper + anchoring bits in
 * popout.c/.h); control-center/dashboard/etc. convert in T3+.
 */
#ifndef DC_UI_CONNECTED_H
#define DC_UI_CONNECTED_H

#include <stdbool.h>

struct dc_render;
typedef struct NVGcontext NVGcontext;

/* Draw the panel card chrome into a `w`x`h` logical popout surface.
 * `bottom_bar` selects which edge is the "near" (bar-facing) edge: false ->
 * near edge is the top of the surface (bar at DC_BAR_POSITION_TOP, popout
 * opens below it); true -> near edge is the bottom (bar at
 * DC_BAR_POSITION_BOTTOM, popout opens above it). Leaves no path/scissor
 * state dangling on return. */
void dc_connected_card_chrome(NVGcontext *vg, struct dc_render *render, float w, float h,
                              bool bottom_bar);

#endif /* DC_UI_CONNECTED_H */
