/* bar.h — the DankBar: one wlr-layer-shell surface per output, anchored to the
 * top edge, GPU-rendered. Milestone 1 draws a solid themed strip; widgets and
 * text arrive in Milestone 2 (see docs/06-ROADMAP.md).
 */
#ifndef DC_UI_BAR_BAR_H
#define DC_UI_BAR_BAR_H

struct dc_wayland;
struct dc_output;
struct dc_egl;

typedef struct dc_bar dc_bar;

/* Create a bar on `output`. Returns NULL on failure. Owned by the caller. */
dc_bar *dc_bar_create(struct dc_wayland *wl, struct dc_output *output, struct dc_egl *egl);
void dc_bar_destroy(dc_bar *bar);

#endif /* DC_UI_BAR_BAR_H */
