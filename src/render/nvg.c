#include "render/nvg.h"

#include "core/log.h"

#include <fontconfig/fontconfig.h>
#include <GLES3/gl3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

#include "nanosvg.h"
#include "nanosvgrast.h"
#include "nanovg.h"
#define NANOVG_GLES3
#include "nanovg_gl.h"

/* Searched in order; first existing wins. Inter is the DankC UI font (bundled),
 * with system fallbacks for a bare dev checkout. */
static const char *const FONT_CANDIDATES[] = {
    "assets/fonts/InterVariable.ttf",
    "/usr/share/dankc/fonts/InterVariable.ttf",
    "/usr/share/fonts/noto/NotoSans-Regular.ttf",
    "/usr/share/fonts/TTF/DejaVuSans.ttf",
};

static const char *const ICON_FONT_CANDIDATES[] = {
    "assets/fonts/MaterialSymbolsRounded.ttf",
    "/usr/share/dankc/fonts/MaterialSymbolsRounded.ttf",
};

/* --- TTF/OTF cmap coverage (docs/12-BAR-SPEC.md: no-tofu for non-Latin text)
 *
 * A minimal, read-only cmap parser: given a font file, build a bitmap of
 * which BMP codepoints (U+0000-U+FFFF) it has a glyph for. dankc never tries
 * to render above the BMP (bar_sanitize_utf8() drops those before consulting
 * this), so the bitmap doesn't bother tracking supplementary planes. Only
 * format 4 (BMP segment mapping) and format 12 (full-Unicode groups) are
 * understood — the two subtable formats every font we've seen (Inter,
 * DejaVu Sans, Noto Sans Arabic, ...) actually ships. Anything else is
 * silently skipped, leaving that font's coverage bitmap all-zero and
 * `valid = false` (fail-open at the call site, see cov_get()). */
#define COV_BYTES 8192 /* 65536 codepoints / 8 bits */

typedef struct {
    unsigned char bits[COV_BYTES];
    bool valid;
} font_coverage;

static font_coverage g_cov_ui;
static font_coverage g_cov_fallback;

static uint16_t be16(const unsigned char *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static uint32_t be32(const unsigned char *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static void cov_set(font_coverage *cov, uint32_t cp)
{
    if (cp > 0xFFFF)
        return;
    cov->bits[cp >> 3] |= (unsigned char)(1u << (cp & 7));
}

/* `fail_open`: what to return when this coverage set was never successfully
 * parsed (cov->valid == false) — true for the primary UI font (an unparsable
 * bundled font shouldn't make every character disappear), false for the
 * optional fallback font (no fallback loaded just means "not covered"). */
static bool cov_get(const font_coverage *cov, uint32_t cp, bool fail_open)
{
    if (!cov->valid)
        return fail_open;
    if (cp > 0xFFFF)
        return false;
    return (cov->bits[cp >> 3] >> (cp & 7)) & 1u;
}

/* cmap format 4: BMP-only segmented mapping (see the OpenType spec's `cmap`
 * chapter). We only need "has a glyph or not", so idDelta/idRangeOffset are
 * followed just far enough to tell a real glyph id from 0 (.notdef). */
static void parse_cmap_format4(const unsigned char *sub, size_t avail, font_coverage *cov)
{
    if (avail < 14)
        return;
    uint16_t seg_x2 = be16(sub + 6);
    uint16_t seg_count = (uint16_t)(seg_x2 / 2);
    const unsigned char *end_codes = sub + 14;
    const unsigned char *start_codes = end_codes + seg_x2 + 2; /* +2 skips reservedPad */
    const unsigned char *id_deltas = start_codes + seg_x2;
    const unsigned char *id_range_offs = id_deltas + seg_x2;
    if ((size_t)(id_range_offs + seg_x2 - sub) > avail)
        return;

    for (uint16_t i = 0; i < seg_count; i++) {
        uint16_t start = be16(start_codes + i * 2);
        uint16_t end = be16(end_codes + i * 2);
        if (start == 0xFFFF && end == 0xFFFF)
            continue; /* terminator segment */
        int16_t delta = (int16_t)be16(id_deltas + i * 2);
        uint16_t range_off = be16(id_range_offs + i * 2);

        for (uint32_t cp = start; cp <= end; cp++) {
            uint16_t glyph;
            if (range_off == 0) {
                glyph = (uint16_t)(cp + delta);
            } else {
                const unsigned char *g = id_range_offs + i * 2 + range_off + (cp - start) * 2;
                if ((size_t)(g + 2 - sub) > avail)
                    glyph = 0;
                else {
                    glyph = be16(g);
                    if (glyph != 0)
                        glyph = (uint16_t)(glyph + delta);
                }
            }
            if (glyph != 0)
                cov_set(cov, cp);
        }
    }
}

/* cmap format 12: full-Unicode {startCharCode, endCharCode, startGlyphID}
 * groups. */
static void parse_cmap_format12(const unsigned char *sub, size_t avail, font_coverage *cov)
{
    if (avail < 16)
        return;
    uint32_t n_groups = be32(sub + 12);
    for (uint32_t i = 0; i < n_groups; i++) {
        const unsigned char *g = sub + 16 + (size_t)i * 12;
        if ((size_t)(g + 12 - sub) > avail)
            break;
        uint32_t start = be32(g);
        uint32_t end = be32(g + 4);
        if (start > 0xFFFF)
            continue;
        if (end > 0xFFFF)
            end = 0xFFFF;
        for (uint32_t cp = start; cp <= end; cp++)
            cov_set(cov, cp);
    }
}

/* Read `path`'s sfnt table directory, find the best available Unicode cmap
 * subtable (format 12 preferred over format 4), and fill `cov`. Best-effort:
 * `cov->valid` stays false on any I/O or parse problem. */
static void load_cmap(const char *path, font_coverage *cov)
{
    memset(cov, 0, sizeof(*cov));

    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 12 || sz > (16 << 20)) {
        fclose(f);
        return;
    }
    unsigned char *data = malloc((size_t)sz);
    bool ok = data && fread(data, 1, (size_t)sz, f) == (size_t)sz;
    fclose(f);
    if (!ok) {
        free(data);
        return;
    }

    size_t n = (size_t)sz;
    uint16_t num_tables = be16(data + 4);
    uint32_t cmap_off = 0;
    for (uint16_t i = 0; i < num_tables && 12u + (uint32_t)i * 16u + 16u <= n; i++) {
        const unsigned char *rec = data + 12 + i * 16;
        if (memcmp(rec, "cmap", 4) == 0) {
            cmap_off = be32(rec + 8);
            break;
        }
    }
    if (!cmap_off || (size_t)cmap_off + 4 > n)
        goto out;

    uint16_t cmap_tables = be16(data + cmap_off + 2);
    uint32_t best_off = 0, best_fmt = 0;
    for (uint16_t i = 0; i < cmap_tables && (size_t)cmap_off + 4 + (uint32_t)i * 8u + 8u <= n; i++) {
        const unsigned char *rec = data + cmap_off + 4 + i * 8;
        uint16_t plat = be16(rec), enc = be16(rec + 2);
        uint32_t off = be32(rec + 4);
        if ((size_t)cmap_off + off + 2 > n)
            continue;
        bool unicode = (plat == 3 && (enc == 1 || enc == 10)) || plat == 0;
        if (!unicode)
            continue;
        uint16_t fmt = be16(data + cmap_off + off);
        if (fmt != 4 && fmt != 12)
            continue;
        if (fmt == 12 || best_fmt != 12) { /* format 12 always wins; else first format-4 found */
            best_off = cmap_off + off;
            best_fmt = fmt;
        }
    }
    if (!best_off)
        goto out;

    size_t avail = n - best_off;
    if (best_fmt == 4)
        parse_cmap_format4(data + best_off, avail, cov);
    else
        parse_cmap_format12(data + best_off, avail, cov);
    cov->valid = true;

out:
    free(data);
}

/* fontconfig lookup for a fallback family covering Arabic script (the user's
 * language — Urdu shares the Arabic block). Linked at build time (see
 * meson.build/Makefile); FcInit()/FcFini() bracket a single one-shot query at
 * startup, not held open for the process lifetime. Rendering is unshaped
 * (letters stay in isolated form rather than joining) since dankc has no
 * text-shaping engine — still far better than a tofu box. Returns a
 * heap-allocated path (caller frees) or NULL if fontconfig found nothing. */
static char *find_fallback_font_path(void)
{
    if (!FcInit()) {
        dc_warn("fontconfig init failed; no non-Latin fallback font");
        return NULL;
    }

    char *result = NULL;
    FcPattern *pat = FcNameParse((const FcChar8 *)"sans:lang=ar");
    if (pat) {
        FcConfigSubstitute(NULL, pat, FcMatchPattern);
        FcDefaultSubstitute(pat);
        FcResult fc_result;
        FcPattern *match = FcFontMatch(NULL, pat, &fc_result);
        if (match) {
            FcChar8 *file = NULL;
            if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch && file)
                result = strdup((const char *)file);
            FcPatternDestroy(match);
        }
        FcPatternDestroy(pat);
    }

    FcFini();
    return result;
}

static int load_font(NVGcontext *vg, const char *name, const char *const *candidates, size_t count,
                     const char **out_path)
{
    for (size_t i = 0; i < count; i++) {
        if (access(candidates[i], R_OK) != 0)
            continue;
        int font = nvgCreateFont(vg, name, candidates[i]);
        if (font >= 0) {
            dc_debug("loaded font '%s': %s", name, candidates[i]);
            if (out_path)
                *out_path = candidates[i];
            return font;
        }
    }
    return -1;
}

/* Encode a Unicode codepoint as UTF-8. Returns the byte count. */
static int utf8_encode(int cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xc0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3f));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xe0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3f));
        out[2] = (char)(0x80 | (cp & 0x3f));
        return 3;
    }
    out[0] = (char)(0xf0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3f));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3f));
    out[3] = (char)(0x80 | (cp & 0x3f));
    return 4;
}

bool dc_render_ensure(dc_render *render)
{
    if (render->ready)
        return true;

    render->vg = nvgCreateGLES3(NVG_ANTIALIAS | NVG_STENCIL_STROKES);
    if (!render->vg) {
        dc_error("nvgCreateGLES3 failed");
        return false;
    }

    const char *ui_path = NULL;
    render->font_ui = load_font(render->vg, "ui", FONT_CANDIDATES,
                                sizeof(FONT_CANDIDATES) / sizeof(char *), &ui_path);
    if (render->font_ui < 0) {
        dc_error("no usable UI font found");
        nvgDeleteGLES3(render->vg);
        render->vg = NULL;
        return false;
    }
    if (ui_path)
        load_cmap(ui_path, &g_cov_ui);
    if (!g_cov_ui.valid)
        dc_warn("could not parse UI font cmap; non-Latin coverage check disabled (fail-open)");

    render->font_icons = load_font(render->vg, "icons", ICON_FONT_CANDIDATES,
                                   sizeof(ICON_FONT_CANDIDATES) / sizeof(char *), NULL);
    if (render->font_icons < 0)
        dc_warn("Material Symbols icon font not found; icons disabled");

    /* Non-Latin fallback (docs/12-BAR-SPEC.md: no-tofu). Added via
     * nvgAddFallbackFontId() so it's ONLY consulted for glyphs `font_ui`
     * lacks — Inter's own Latin glyphs are always used first, so this can't
     * change how Latin text renders. */
    render->font_fallback = -1;
    char *fallback_path = find_fallback_font_path();
    if (fallback_path) {
        int fb = nvgCreateFont(render->vg, "fallback", fallback_path);
        if (fb >= 0 && nvgAddFallbackFontId(render->vg, render->font_ui, fb)) {
            render->font_fallback = fb;
            load_cmap(fallback_path, &g_cov_fallback);
            dc_info("non-Latin fallback font: %s", fallback_path);
        } else {
            dc_warn("could not load fallback font %s", fallback_path);
        }
        free(fallback_path);
    } else {
        dc_warn("fontconfig found no Arabic-covering fallback font; non-Latin text will be dropped");
    }

    render->ready = true;
    dc_debug("nanovg render context ready");
    return true;
}

bool dc_render_font_has(uint32_t codepoint)
{
    if (cov_get(&g_cov_ui, codepoint, true))
        return true;
    return cov_get(&g_cov_fallback, codepoint, false);
}

void dc_render_icon(dc_render *render, int codepoint, float x, float y, float size, dc_color color,
                    int align_nvg)
{
    if (!render->vg || render->font_icons < 0)
        return;

    char glyph[5];
    int len = utf8_encode(codepoint, glyph);
    glyph[len] = '\0';

    nvgFontFaceId(render->vg, render->font_icons);
    nvgFontSize(render->vg, size);
    nvgFillColor(render->vg, nvgRGBA(color.r, color.g, color.b, color.a));
    nvgTextAlign(render->vg, align_nvg);
    nvgText(render->vg, x, y, glyph, NULL);
}

int dc_render_load_icon(dc_render *render, const char *path, int size)
{
    if (!render->vg || !path || size <= 0)
        return 0;

    size_t len = strlen(path);
    if (len > 4 && strcasecmp(path + len - 4, ".svg") == 0) {
        NSVGimage *svg = nsvgParseFromFile(path, "px", 96.0f);
        if (!svg)
            return 0;
        float src = svg->width > svg->height ? svg->width : svg->height;
        float scale = src > 0.0f ? (float)size / src : 1.0f;

        unsigned char *pixels = calloc((size_t)size * size * 4, 1);
        if (!pixels) {
            nsvgDelete(svg);
            return 0;
        }
        NSVGrasterizer *rast = nsvgCreateRasterizer();
        nsvgRasterize(rast, svg, 0.0f, 0.0f, scale, pixels, size, size, size * 4);
        int handle = nvgCreateImageRGBA(render->vg, size, size, 0, pixels);
        nsvgDeleteRasterizer(rast);
        free(pixels);
        nsvgDelete(svg);
        return handle;
    }

    return nvgCreateImage(render->vg, path, 0);
}

void dc_render_finish(dc_render *render)
{
    if (render->vg)
        nvgDeleteGLES3(render->vg);
    render->vg = NULL;
    render->ready = false;
    render->font_fallback = -1;
    g_cov_ui.valid = false;
    g_cov_fallback.valid = false;
}
