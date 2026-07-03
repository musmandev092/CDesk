/* frame.h — DMS "Frame": rounded screen corners.
 *
 * A thin, always-on-top, click-through layer-shell overlay per output that
 * paints 4 opaque corner "bites" so the rectangular screen reads as rounded.
 * Static content: rendered once on configure / theme / config change, never
 * per-frame (docs/POLISH.md P2). This intentionally does NOT implement DMS's
 * full "connected chrome" Frame system (docs/11-UX-FLOW.md sec.4) — just the
 * corner-rounding effect, which is what's visually noticeable.
 */
#ifndef DC_UI_FRAME_H
#define DC_UI_FRAME_H

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;

typedef struct dc_frame dc_frame;

/* One frame overlay for `output`. Shown immediately if dc_config_current->
 * frame_enabled is true; otherwise created inert (no surface) until the next
 * dc_frame_reconfigure() flips it on. */
dc_frame *dc_frame_create(struct dc_wayland *wl, struct dc_output *output, struct dc_egl *egl,
                          struct dc_render *render);
void dc_frame_destroy(dc_frame *f);

/* Re-read dc_config_current (frame_enabled/frame_radius) and the active theme,
 * showing/hiding/repainting as needed. Cheap to call on every config-changed
 * notification — repaints happen only when something actually changed. */
void dc_frame_reconfigure(dc_frame *f);

#endif /* DC_UI_FRAME_H */
