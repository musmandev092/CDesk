/* nvg.h — the nanovg render context (GPU 2D + text).
 *
 * One dc_render is shared across all bars/panels because they share a single
 * GL context. dc_render_ensure() must be called with that context current.
 */
#ifndef DC_RENDER_NVG_H
#define DC_RENDER_NVG_H

#include <stdbool.h>

#include "theme/theme.h"

typedef struct NVGcontext NVGcontext;

typedef struct dc_render {
    NVGcontext *vg;
    int font_ui;
    int font_icons; /* Material Symbols Rounded; -1 if unavailable */
    bool ready;
} dc_render;

/* Draw a Material Symbols icon (by codepoint) at (x, y) with the given nanovg
 * alignment. No-op if the icon font failed to load. */
void dc_render_icon(dc_render *render, int codepoint, float x, float y, float size, dc_color color,
                    int align_nvg);

/* Load an image icon from a file path into a nanovg image handle. Handles PNG
 * (via stb_image) and SVG (via nanosvg, rasterised to `size`x`size`). Returns
 * the handle (>0) or 0 on failure. Caller frees with nvgDeleteImage. */
int dc_render_load_icon(dc_render *render, const char *path, int size);

/* Lazily create the nanovg context and load the UI font. Idempotent. Must run
 * with a GL context current. Returns false on failure. */
bool dc_render_ensure(dc_render *render);
void dc_render_finish(dc_render *render);

#endif /* DC_RENDER_NVG_H */
