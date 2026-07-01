/* controlcenter.h — the Control Center popout.
 *
 * A wlr-layer-shell overlay surface anchored top-right (under the bar's
 * control-center area), rendered with nanovg as a themed card. Opened/closed by
 * clicking the bar's control-center region. Toggles + sliders are added
 * incrementally (see docs/04-FEATURES §3).
 */
#ifndef DC_UI_CONTROLCENTER_H
#define DC_UI_CONTROLCENTER_H

#include <stdbool.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;

typedef struct dc_control_center dc_control_center;

dc_control_center *dc_control_center_create(struct dc_wayland *wl, struct dc_egl *egl,
                                            struct dc_render *render);
void dc_control_center_destroy(dc_control_center *cc);

/* Show on `output` if hidden, hide if shown. */
void dc_control_center_toggle(dc_control_center *cc, struct dc_output *output);
bool dc_control_center_visible(dc_control_center *cc);

#endif /* DC_UI_CONTROLCENTER_H */
