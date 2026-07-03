/* shape.h — complex text shaping (Arabic/Urdu joining) + BiDi (RTL) layer.
 *
 * nanovg's text path (see nvg.c) is fontstash: a 1:1 UTF-8 codepoint -> glyph
 * cmap lookup, drawn in logical string order. That's correct for Latin but
 * wrong for Arabic-script text two ways: (1) Arabic letters have up to four
 * contextual joined forms (isolated/initial/medial/final) selected by
 * OpenType GSUB features fontstash never applies, so codepoint-per-glyph
 * rendering shows disconnected isolated letterforms; (2) RTL text needs the
 * Unicode Bidirectional Algorithm (UAX#9) to compute visual glyph order,
 * which a plain left-to-right draw loop doesn't do at all.
 *
 * This module is a drop-in wrapper around nvgText/nvgTextBounds/nvgTextBox:
 * dc_shape_needed() is a fast, allocation-free scan that gates the whole
 * thing — plain Latin/digits/punctuation text (the overwhelming common case
 * for a status bar) takes nanovg's original codepoint path completely
 * unchanged, so there is zero added cost for the common case. Text
 * containing an RTL/joining-script codepoint instead runs FriBidi (bidi
 * reordering into visual runs) + HarfBuzz (per-run shaping: glyph selection
 * + positioning) and renders through nanovg's patched nvgTextGlyphs() glyph-
 * index entry point (see third_party/nanovg/{nanovg,fontstash}.h).
 *
 * All entry points take the existing dc_render* so the shaper can resolve
 * the *same* font file nvg's own glyph-coverage fallback chain would have
 * picked for a given codepoint (dc_render_font_for_codepoint()) — shaped and
 * unshaped text stay metrically consistent even when they share a line.
 *
 * Shaped results are cached (keyed on the exact text + font size) with a
 * small LRU, since bar widgets and toasts re-render every frame but their
 * text rarely changes between frames.
 */
#ifndef DC_RENDER_SHAPE_H
#define DC_RENDER_SHAPE_H

#include <stddef.h>

#include "render/nvg.h"

/* True if `text` ([text, end), or NUL-terminated if end is NULL) contains any
 * codepoint from a script that needs BiDi reordering and/or contextual glyph
 * joining: Arabic (U+0600-06FF, U+0750-077F, U+08A0-08FF, U+FB50-FDFF,
 * U+FE70-FEFF) or Hebrew (U+0590-05FF). O(n) over the UTF-8 bytes, no
 * allocation — safe to call every frame as a gate before the expensive path. */
bool dc_shape_needed(const char *text, const char *end);

/* Drop-in replacement for nvgText(): must be called with the same nvg state
 * already set (nvgFontFaceId/nvgFontSize/nvgTextAlign/nvgFillColor) exactly
 * like a plain nvgText() call. Internally: if !dc_shape_needed(text), calls
 * nvgText() directly (identical output, identical cost). Otherwise shapes
 * `text` (BiDi + HarfBuzz, cached) and draws it via nvgTextGlyphs(), which
 * may switch the bound font face mid-draw for RTL runs (e.g. to the Arabic
 * fallback font) — the face is always restored to whatever was bound on
 * entry before returning, so callers never see a leaked font face. Honors
 * horizontal align (left/center/right) by pre-offsetting from the shaped
 * text's total width, matching nvgText's own align handling. Returns the x
 * position where the next glyph would start, like nvgText(). */
float dc_shape_draw_text(dc_render *render, float x, float y, const char *text, const char *end);

/* Drop-in replacement for nvgTextBounds(): same shaped-vs-plain gate as
 * dc_shape_draw_text(), same measurement (sum of the shaper's advances) so
 * width-dependent layout (bar column widths, ellipsis budgets, hit-testing)
 * stays correct for shaped text. `bounds` (if non-NULL) receives
 * [minx,miny,maxx,maxy] like nvgTextBounds(); only the x extent differs
 * between the two paths; y extent comes from nvgTextMetrics(), same as
 * nanovg's own implementation. Returns the horizontal advance. */
float dc_shape_text_bounds(dc_render *render, float x, float y, const char *text, const char *end,
                           float *bounds);

/* Drop-in replacement for nvgTextBox(): line-break positions are computed by
 * nanovg's own nvgTextBreakLines() (an approximation for shaped text — its
 * per-row width is measured on the *unshaped* codepoint advances, so a
 * ligature/joining-narrowed row may wrap a little early; acceptable for
 * notification/clipboard body text where exact fill isn't load-bearing), but
 * each resulting row is then drawn with dc_shape_draw_text() so the actual
 * glyphs are properly joined/reordered. */
void dc_shape_draw_textbox(dc_render *render, float x, float y, float break_row_width, const char *text,
                           const char *end);

/* Ellipsize `text` (current nvg font/size) to fit `max_width`, writing the
 * possibly-truncated, NUL-terminated UTF-8 result into `out` (capacity
 * `out_sz`) and returning its shaped/measured width. No-op copy (just a
 * width measurement) if it already fits. Implemented as "truncate the
 * logical string and append an ellipsis", exactly like the plain-Latin
 * bar_ellipsize() it's meant to replace — for an RTL paragraph this
 * automatically lands the ellipsis on the visual *left* (BiDi resolves the
 * trailing neutral "…" into the surrounding RTL run, which occupies the
 * left end of the line), matching Qt/DMS elide-right semantics without any
 * script-specific casing. Safe to call on plain Latin text too (cheap
 * fallback via dc_shape_needed()). */
float dc_shape_ellipsize(dc_render *render, const char *text, float max_width, char *out, size_t out_sz);

/* Drop every cached shaped-text entry and any cached HarfBuzz font objects.
 * Call after a font/theme reload (dc_render_finish()+dc_render_ensure()) so
 * stale glyph ids from a torn-down nvg font context can't leak into a new
 * one. */
void dc_shape_reset_cache(void);

#endif /* DC_RENDER_SHAPE_H */
