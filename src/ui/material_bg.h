/* material_bg.h — the "material" blurred-wallpaper panel background.
 *
 * DMS panels sit on a blurred+dimmed copy of the wallpaper instead of a flat
 * surfaceContainer fill (docs/POLISH.md P2, docs/11-UX-FLOW.md sec.2). This
 * is a cheap CPU approximation: the wallpaper is decoded once, box-sampled
 * down to a tiny (~1/8) texture (a heavily-downscaled-then-bilinear-upscaled
 * image reads as a soft blur), and cached as an nvg image. Panels draw it
 * stretched to their card rect (clipped to the rounded corners) with a
 * themed scrim on top for legibility, instead of a flat fill.
 *
 * One shared cache for the whole process (all panels/outputs draw the same
 * cached texture stretched to their own rect — the source is already blurred
 * past the point where per-output/per-position accuracy matters).
 */
#ifndef DC_UI_MATERIAL_BG_H
#define DC_UI_MATERIAL_BG_H

struct dc_render;
typedef struct NVGcontext NVGcontext;

/* Drop-in replacement for the "nvgBeginPath + nvgRoundedRect(x,y,w,h,radius)
 * + nvgFillColor(surfaceContainer) + nvgFill" 4-liner every popout card uses.
 * When dc_config_current->material_blur is on and a wallpaper is configured
 * and readable, fills the rounded rect with the cached blurred+dimmed
 * wallpaper texture; otherwise (disabled, no wallpaper, or decode failure)
 * falls back to the identical flat surfaceContainer fill, silently (no
 * warning spam — a missing/unset wallpaper is a normal, common case). Leaves
 * the rounded-rect path current on return, so a caller's subsequent
 * nvgStrokeColor()/nvgStroke() for the card border still works unchanged. */
void dc_material_bg_fill_card(NVGcontext *vg, struct dc_render *render, float x, float y, float w,
                              float h, float radius);

/* Drop the cached texture so the next dc_material_bg_fill_card() call
 * regenerates it from disk. Call after the configured wallpaper path
 * changes (config load/reapply, wallpaper picker). Safe to call even if
 * nothing was ever cached. */
void dc_material_bg_invalidate(void);

#endif /* DC_UI_MATERIAL_BG_H */
