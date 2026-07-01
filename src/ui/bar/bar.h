/* bar.h — the DankBar: one wlr-layer-shell surface per output, anchored to the
 * top edge, GPU-rendered via nanovg. Milestone 2 draws the themed background
 * plus a live clock; workspaces and more widgets follow.
 */
#ifndef DC_UI_BAR_BAR_H
#define DC_UI_BAR_BAR_H

struct dc_wayland;
struct dc_output;
struct dc_egl;
struct dc_render;
struct dc_niri;

typedef struct dc_bar dc_bar;

/* Create a bar on `output`. `niri` may be NULL (workspaces then omitted).
 * Returns NULL on failure. Owned by the caller. */
dc_bar *dc_bar_create(struct dc_wayland *wl, struct dc_output *output, struct dc_egl *egl,
                      struct dc_render *render, struct dc_niri *niri);
void dc_bar_destroy(dc_bar *bar);

/* Re-render the bar (e.g. on a clock tick or a compositor event). No-op until
 * the surface has been configured. */
void dc_bar_render(dc_bar *bar);

#endif /* DC_UI_BAR_BAR_H */
