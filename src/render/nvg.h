/* nvg.h — the nanovg render context (GPU 2D + text).
 *
 * One dc_render is shared across all bars/panels because they share a single
 * GL context. dc_render_ensure() must be called with that context current.
 */
#ifndef DC_RENDER_NVG_H
#define DC_RENDER_NVG_H

#include <stdbool.h>
#include <stdint.h>

#include "theme/theme.h"

typedef struct NVGcontext NVGcontext;

/* Upper bound on fontconfig/vendored fallback fonts chained after the UI
 * font (script coverage + monochrome emoji; see nvg.c's FALLBACK_SPECS). */
#define DC_RENDER_MAX_FALLBACKS 8

/* Opaque per-font cmap coverage bitmap; real definition (and the parser) is
 * private to nvg.c. Kept alive for the process lifetime (one per loaded font)
 * so render/shape.c can ask "does font X actually have a glyph for this
 * specific codepoint" without re-parsing the font file on every shaped
 * string — see dc_render_font_for_codepoint(). */
typedef struct dc_font_coverage dc_font_coverage;

typedef struct dc_render {
    NVGcontext *vg;
    int font_ui;
    int font_icons; /* Material Symbols Rounded; -1 if unavailable */
    /* Script/emoji fallbacks chained onto font_ui via nvgAddFallbackFontId()
     * (CJK, Devanagari, Thai, Cyrillic/Greek, Arabic, monochrome NotoEmoji —
     * see nvg.c). Handles kept for bookkeeping only; text rendering goes
     * through font_ui and falls back automatically. */
    int font_fallbacks[DC_RENDER_MAX_FALLBACKS];
    int font_fallback_count;
    /* File path + parsed cmap coverage backing font_ui / each font_fallbacks
     * entry, in the same order/index. render/shape.c (HarfBuzz shaping) uses
     * these to (a) open the exact same font bytes nvg's stb_truetype loaded,
     * so glyph ids line up, and (b) pick the right one per codepoint via
     * dc_render_font_for_codepoint() — the same "first font in the chain
     * that covers it" rule fontstash's own fallback resolution uses. */
    char font_ui_path[256];
    dc_font_coverage *font_ui_cov;
    char font_fallback_paths[DC_RENDER_MAX_FALLBACKS][256];
    dc_font_coverage *font_fallback_cov[DC_RENDER_MAX_FALLBACKS];
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

/* True if the UI font or ANY loaded fallback font (see nvg.c) has a glyph
 * for `codepoint` — full Unicode range, supplementary planes (emoji)
 * included. Used by bar.c's title sanitizer to drop codepoints that would
 * otherwise draw as a "tofu" .notdef box instead of falling back cleanly.
 * Fails open (returns true) before dc_render_ensure() has run or if the UI
 * font's cmap couldn't be parsed, so it never strips text it can't
 * positively rule out. */
bool dc_render_font_has(uint32_t codepoint);

/* Which loaded font (font_ui first, then font_fallbacks[] in registered
 * order — the exact order/priority fontstash's own glyph-index fallback
 * search uses) actually has a glyph for `codepoint`. Returns the nvg font id
 * (>= 0, suitable for nvgFontFaceId()/nvgTextGlyphs()) and, if `out_path` is
 * non-NULL, the font file path backing it (for opening the same bytes with
 * HarfBuzz). Returns -1 if no loaded font covers it. Used by render/shape.c
 * so a HarfBuzz-shaped run targets the identical font+glyph-index space
 * nanovg's own unshaped fallback path would have used, keeping metrics
 * (advances, atlas entries) consistent between the two draw paths. */
int dc_render_font_for_codepoint(dc_render *render, uint32_t codepoint, const char **out_path);

#endif /* DC_RENDER_NVG_H */
