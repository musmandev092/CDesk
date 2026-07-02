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
 * which codepoints (full Unicode, U+0000-U+10FFFF — supplementary planes
 * included, so emoji coverage from the vendored monochrome NotoEmoji is
 * tracked too) it has a glyph for. Only format 4 (BMP segment mapping) and
 * format 12 (full-Unicode groups) are understood — the two subtable formats
 * every font we've seen (Inter, DejaVu Sans, Noto Sans *, Noto Emoji, ...)
 * actually ships. Anything else is silently skipped, leaving that font's
 * coverage bitmap all-zero and `valid = false`.
 *
 * TrueType Collections (.ttc, e.g. Noto Sans CJK) are supported by reading
 * the first font's table directory — matching fontstash/stb_truetype, which
 * only ever loads collection index 0 through nvgCreateFont().
 *
 * Per-font bitmaps are heap-allocated transiently at startup and OR-ed into
 * one static union bitmap (g_cov, 136 KiB): dc_render_font_has() only ever
 * needs "does ANY loaded font cover this codepoint". */
#define COV_MAX_CP 0x110000u
#define COV_BYTES (COV_MAX_CP / 8) /* 136 KiB */

typedef struct {
    unsigned char bits[COV_BYTES];
    bool valid;
} font_coverage;

/* Union of every successfully parsed loaded font. `ui_valid` records whether
 * the primary UI font's cmap parsed: if it didn't, dc_render_font_has() fails
 * open (an unparsable bundled font shouldn't make every character vanish). */
static struct {
    unsigned char bits[COV_BYTES];
    bool ui_valid;
} g_cov;

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
    if (cp >= COV_MAX_CP)
        return;
    cov->bits[cp >> 3] |= (unsigned char)(1u << (cp & 7));
}

static bool cov_get(const font_coverage *cov, uint32_t cp)
{
    if (cp >= COV_MAX_CP)
        return false;
    return (cov->bits[cp >> 3] >> (cp & 7)) & 1u;
}

/* OR a successfully parsed per-font bitmap into the global union. */
static void cov_merge(const font_coverage *cov)
{
    for (size_t i = 0; i < COV_BYTES; i++)
        g_cov.bits[i] |= cov->bits[i];
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
        if (start >= COV_MAX_CP)
            continue;
        if (end >= COV_MAX_CP)
            end = COV_MAX_CP - 1;
        for (uint32_t cp = start; cp <= end; cp++)
            cov_set(cov, cp);
    }
}

/* Read `path`'s sfnt table directory, find the best available Unicode cmap
 * subtable (format 12 preferred over format 4), and fill `cov`. For .ttc
 * collections, the FIRST font's table directory is used (see the coverage
 * block comment above — fontstash only loads collection index 0). Best-
 * effort: `cov->valid` stays false on any I/O or parse problem. */
static void load_cmap(const char *path, font_coverage *cov)
{
    memset(cov, 0, sizeof(*cov));

    FILE *f = fopen(path, "rb");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (sz < 16 || sz > (64L << 20)) { /* Noto Sans CJK .ttc is ~20 MB */
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
    /* TrueType Collection: hop to the first member font's table directory. */
    size_t dir = 0;
    if (memcmp(data, "ttcf", 4) == 0) {
        uint32_t first = be32(data + 12); /* offsetTable[0] */
        if ((size_t)first + 12 > n)
            goto out;
        dir = first;
    }

    uint16_t num_tables = be16(data + dir + 4);
    uint32_t cmap_off = 0;
    for (uint16_t i = 0; i < num_tables && dir + 12u + (uint32_t)i * 16u + 16u <= n; i++) {
        const unsigned char *rec = data + dir + 12 + i * 16;
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

/* --- fallback font chain (docs/12-BAR-SPEC.md: no-tofu for non-Latin text)
 *
 * fontconfig-located script fallbacks, chained after Inter via
 * nvgAddFallbackFontId() so they're ONLY consulted for glyphs the UI font
 * lacks. Rendering is unshaped (Arabic letters stay in isolated form,
 * Devanagari conjuncts don't form) since dankc has no text-shaping engine —
 * still far better than a tofu box.
 *
 * Each pattern carries a probe codepoint: if the matched file's cmap lacks
 * it, the match is rejected (guards against fontconfig handing back a file
 * whose collection index 0 — the only face stb_truetype reads — doesn't
 * actually cover the script) and `retry_path` is tried instead, if set.
 *
 * Emoji come from the vendored monochrome NotoEmoji (assets/fonts/) — the
 * distro noto-fonts-emoji is COLOR (CBDT bitmaps, no outlines), which
 * stb_truetype cannot rasterise, so emoji intentionally render MONOCHROME.
 * Variation selectors/ZWJ are stripped by bar_sanitize_utf8(), so ZWJ
 * sequences decompose into their component emoji. */
typedef struct {
    const char *pattern;    /* fontconfig pattern, or NULL: `retry_path` only */
    uint32_t probe;         /* codepoint the matched font must cover; 0 = any */
    const char *retry_path; /* literal fallback path if the fc match fails */
} fallback_spec;

static const fallback_spec FALLBACK_SPECS[] = {
    {"sans", 0x0410, NULL},          /* general: Cyrillic/Greek/... (Noto Sans) */
    {"sans:lang=ar", 0x0627, NULL},  /* Arabic/Urdu (existing behavior) */
    {":lang=zh-cn", 0x4F60, "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"},
    {":lang=ja", 0x3053, "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"},
    {":lang=ko", 0xC548, "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc"},
    {":lang=hi", 0x0928, NULL},      /* Devanagari */
    {":lang=th", 0x0E2A, NULL},      /* Thai */
};

/* Vendored monochrome emoji font, appended after the fontconfig chain. */
static const char *const EMOJI_FONT_CANDIDATES[] = {
    "assets/fonts/NotoEmoji-Regular.ttf",
    "/usr/share/dankc/fonts/NotoEmoji-Regular.ttf",
};

#define DC_MAX_FALLBACK_FONTS DC_RENDER_MAX_FALLBACKS

/* One fc-match query. Caller brackets with FcInit()/FcFini(). Returns a
 * heap-allocated file path (caller frees) or NULL. */
static char *fc_match_path(const char *pattern)
{
    char *result = NULL;
    FcPattern *pat = FcNameParse((const FcChar8 *)pattern);
    if (!pat)
        return NULL;
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
    return result;
}

/* Load `path` as one more fallback of the UI font, unless it's already
 * loaded (dedupe — zh-cn/ja/ko usually all resolve to the same CJK .ttc),
 * fails the `probe` coverage check, or the chain is full. On success its
 * cmap coverage is merged into g_cov. `loaded`/`n_loaded` is the dedupe list
 * of paths accepted so far (borrowed pointers, owned by the caller). */
static bool add_fallback_font(dc_render *render, const char *path, uint32_t probe,
                              char loaded[][256], int *n_loaded)
{
    if (!path || render->font_fallback_count >= DC_MAX_FALLBACK_FONTS)
        return false;
    for (int i = 0; i < *n_loaded; i++)
        if (strcmp(loaded[i], path) == 0)
            return true; /* already in the chain; nothing to do */
    if (*n_loaded >= DC_MAX_FALLBACK_FONTS)
        return false;

    font_coverage *cov = malloc(sizeof(*cov));
    if (!cov)
        return false;
    load_cmap(path, cov);
    if (!cov->valid || (probe && !cov_get(cov, probe))) {
        if (cov->valid)
            dc_warn("fallback font %s lacks probe glyph U+%04X; skipped", path, probe);
        free(cov);
        return false;
    }

    int fb = nvgCreateFont(render->vg, path, path); /* name just needs uniqueness */
    if (fb < 0 || !nvgAddFallbackFontId(render->vg, render->font_ui, fb)) {
        dc_warn("could not load fallback font %s", path);
        free(cov);
        return false;
    }

    cov_merge(cov);
    free(cov);
    render->font_fallbacks[render->font_fallback_count++] = fb;
    snprintf(loaded[(*n_loaded)++], 256, "%s", path);
    dc_info("fallback font %d: %s", render->font_fallback_count, path);
    return true;
}

/* Resolve and load the whole fallback chain (script fonts via fontconfig,
 * then the vendored emoji font). Every step is best-effort: a missing font
 * just leaves that script uncovered, and bar_sanitize_utf8() collapses it
 * to "…" instead of tofu. */
static void load_fallback_fonts(dc_render *render)
{
    char loaded[DC_MAX_FALLBACK_FONTS][256];
    int n_loaded = 0;

    if (!FcInit()) {
        dc_warn("fontconfig init failed; no non-Latin fallback fonts");
    } else {
        for (size_t i = 0; i < sizeof(FALLBACK_SPECS) / sizeof(FALLBACK_SPECS[0]); i++) {
            const fallback_spec *spec = &FALLBACK_SPECS[i];
            char *path = fc_match_path(spec->pattern);
            bool ok = path && add_fallback_font(render, path, spec->probe, loaded, &n_loaded);
            free(path);
            if (!ok && spec->retry_path && access(spec->retry_path, R_OK) == 0)
                add_fallback_font(render, spec->retry_path, spec->probe, loaded, &n_loaded);
        }
        FcFini();
    }

    for (size_t i = 0; i < sizeof(EMOJI_FONT_CANDIDATES) / sizeof(char *); i++) {
        if (access(EMOJI_FONT_CANDIDATES[i], R_OK) != 0)
            continue;
        if (add_fallback_font(render, EMOJI_FONT_CANDIDATES[i], 0x1F600, loaded, &n_loaded))
            break;
    }

    if (render->font_fallback_count == 0)
        dc_warn("no fallback fonts loaded; non-Latin text and emoji will be dropped");
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
    /* Contract: a `true` return guarantees `render->vg != NULL`. Every caller
     * (bar.c and every popout) proceeds straight into nanovg on the strength
     * of this, so the `ready` flag and `vg` must never diverge. Verify both,
     * not just `ready`: a NULL vg with `ready` still set (e.g. a future
     * partial-teardown path) would otherwise slip a NULL context into
     * nvgBeginFrame() — the exact fault behind the dc_bar_render->nvgSave
     * startup SIGSEGV (NULL NVGcontext, fault at &ctx->nstates). Treat any
     * such state as not-ready and rebuild. */
    if (render->ready && render->vg)
        return true;
    render->ready = false;

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
    if (ui_path) {
        font_coverage *cov = malloc(sizeof(*cov));
        if (cov) {
            load_cmap(ui_path, cov);
            if (cov->valid) {
                cov_merge(cov);
                g_cov.ui_valid = true;
            }
            free(cov);
        }
    }
    if (!g_cov.ui_valid)
        dc_warn("could not parse UI font cmap; non-Latin coverage check disabled (fail-open)");

    render->font_icons = load_font(render->vg, "icons", ICON_FONT_CANDIDATES,
                                   sizeof(ICON_FONT_CANDIDATES) / sizeof(char *), NULL);
    if (render->font_icons < 0)
        dc_warn("Material Symbols icon font not found; icons disabled");

    /* Script + emoji fallbacks (docs/12-BAR-SPEC.md: no-tofu). Added via
     * nvgAddFallbackFontId() so they're ONLY consulted for glyphs `font_ui`
     * lacks — Inter's own Latin glyphs are always used first, so this can't
     * change how Latin text renders. */
    render->font_fallback_count = 0;
    load_fallback_fonts(render);

    render->ready = true;
    dc_debug("nanovg render context ready");
    return true;
}

bool dc_render_font_has(uint32_t codepoint)
{
    if (!g_cov.ui_valid)
        return true; /* fail open: no coverage data to rule anything out */
    if (codepoint >= COV_MAX_CP)
        return false;
    return (g_cov.bits[codepoint >> 3] >> (codepoint & 7)) & 1u;
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
    render->font_fallback_count = 0;
    memset(&g_cov, 0, sizeof(g_cov));
}
