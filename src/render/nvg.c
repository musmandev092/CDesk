#include "render/nvg.h"

#include "core/log.h"
#include "render/icons.h"
#include "render/shape.h"

#include <fontconfig/fontconfig.h>
#include <GLES3/gl3.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/mman.h>
#include <sys/stat.h>
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

/* docs/POLISH.md P7 item 3: the vendored variable font is 14.5MB, but dankc
 * only ever draws a fixed, small set of codepoints (render/icons.h's
 * DC_ICON_* — see scripts/subset-fonts.sh). A pyftsubset'd copy covering
 * exactly those codepoints is ~248KB and renders identically (same default
 * variable-font instance; no axis coordinates are set anywhere in this
 * file). The subset is preferred; the full font is kept in the repo only as
 * the regeneration source (scripts/subset-fonts.sh reads it) and as a
 * last-resort dev fallback if the subset is ever missing — it is NOT
 * installed (see meson.build), so packaged installs only ship the subset. */
static const char *const ICON_FONT_CANDIDATES[] = {
    "assets/fonts/MaterialSymbolsRounded.subset.ttf",
    "/usr/share/dankc/fonts/MaterialSymbolsRounded.subset.ttf",
    "assets/fonts/MaterialSymbolsRounded.ttf",
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

/* Definition of the opaque dc_font_coverage forward-declared in nvg.h — kept
 * alive (not freed after the startup merge into g_cov) so
 * dc_render_font_for_codepoint() can answer per-font "does THIS specific
 * font cover this codepoint" queries later, for render/shape.c's HarfBuzz
 * font selection. */
struct dc_font_coverage {
    unsigned char bits[COV_BYTES];
    bool valid;
};
typedef struct dc_font_coverage font_coverage;

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
/* docs/16-PERF2-PLAN.md T2.1: lazy fallback-font loading. Scripts this
 * shell's configured user (English UI + Urdu/Arabic, memory
 * `language-urdu-in-english-out`) is unlikely to hit immediately (CJK,
 * Devanagari, Thai, emoji — 46ms of the 108ms cold font-load time, per
 * docs/16 sec.1.2) are deferred until a codepoint from that script is
 * actually about to be rendered; general Sans (Cyrillic/Greek, cheap per
 * docs/16) and Arabic/Urdu (this user's primary non-Latin case — must never
 * tofu on frame 1) stay eager. See dc_render_note_codepoint() (nvg.h). */
typedef enum {
    LAZY_CJK = 0,
    LAZY_DEVANAGARI,
    LAZY_THAI,
    LAZY_TAMIL,
    LAZY_EMOJI,
    LAZY_GROUP_COUNT,
} lazy_group;

/* Bit i set in g_lazy_loaded: that lazy group has been loaded (successfully
 * or not — a failed attempt is not retried every frame, matching the
 * best-effort philosophy of the rest of this file). Bit i set in
 * g_lazy_pending (and NOT in g_lazy_loaded): a codepoint needing that group
 * was seen and hasn't been serviced yet. Both are plain bitmasks — no
 * nanovg/GL state — so they're safe to touch from anywhere, including
 * mid-frame (see dc_render_note_codepoint()'s doc comment in nvg.h). Only
 * `service_lazy_fallbacks()`, called from dc_render_ensure() strictly before
 * every nvgBeginFrame() in this codebase, actually loads a font. */
static unsigned g_lazy_loaded = 0;
static unsigned g_lazy_pending = 0;

/* Unicode block ranges -> which lazy group covers them. Deliberately
 * generous (better to trigger a load a script's outlying codepoints didn't
 * strictly need than to leave a codepoint permanently unrecognised). Returns
 * -1 for anything not lazily managed (Latin/Cyrillic/Greek/Arabic, already
 * eager; or a script this shell has no fallback for at all, unchanged from
 * before this task — those still collapse to "..."/tofu exactly as today). */
static int lazy_group_for_cp(uint32_t cp)
{
    /* CJK: Hiragana/Katakana, CJK Unified + Ext-A, compat ideographs, CJK
     * punctuation, Hangul Jamo + syllables, and the supplementary-plane CJK
     * extensions (Noto Sans CJK covers all of these from one .ttc). */
    if ((cp >= 0x3000 && cp <= 0x303F) || (cp >= 0x3040 && cp <= 0x30FF) ||
        (cp >= 0x3400 && cp <= 0x4DBF) || (cp >= 0x4E00 && cp <= 0x9FFF) ||
        (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0x1100 && cp <= 0x11FF) ||
        (cp >= 0xAC00 && cp <= 0xD7A3) || (cp >= 0x20000 && cp <= 0x2FFFF))
        return LAZY_CJK;
    /* Devanagari + Devanagari Extended. */
    if ((cp >= 0x0900 && cp <= 0x097F) || (cp >= 0xA8E0 && cp <= 0xA8FF))
        return LAZY_DEVANAGARI;
    if (cp >= 0x0E00 && cp <= 0x0E7F)
        return LAZY_THAI;
    /* Tamil + Tamil Supplement. */
    if ((cp >= 0x0B80 && cp <= 0x0BFF) || (cp >= 0x11FC0 && cp <= 0x11FFF))
        return LAZY_TAMIL;
    /* Emoji: misc symbols/dingbats/transport used by monochrome emoji sets,
     * plus the main supplementary-plane emoji blocks (U+1F300 and up, per
     * the task spec). */
    if ((cp >= 0x2300 && cp <= 0x23FF) || (cp >= 0x2600 && cp <= 0x27BF) ||
        (cp >= 0x2B00 && cp <= 0x2BFF) || (cp >= 0x1F000 && cp <= 0x1FFFF))
        return LAZY_EMOJI;
    return -1;
}

bool dc_render_note_codepoint(uint32_t codepoint)
{
    int g = lazy_group_for_cp(codepoint);
    if (g < 0)
        return false;
    if (g_lazy_loaded & (1u << g))
        return false; /* already loaded: plain nvgText's own fallback-chain lookup is safe */
    g_lazy_pending |= (1u << g);
    return true; /* not yet loaded: caller must NOT draw this via plain nvgText (fontstash's
                  * fons__getGlyph() would cache the miss forever — see nvg.h's doc comment) */
}

typedef struct {
    const char *pattern;    /* fontconfig pattern, or NULL: `retry_path` only */
    uint32_t probe;         /* codepoint the matched font must cover; 0 = any */
    const char *retry_path; /* literal fallback path if the fc match fails */
    int lazy_group;         /* -1 = loaded eagerly at startup; else a LAZY_* group */
} fallback_spec;

static const fallback_spec FALLBACK_SPECS[] = {
    {"sans", 0x0410, NULL, -1},          /* general: Cyrillic/Greek/... (Noto Sans) */
    {"sans:lang=ur", 0x06C1, "/usr/share/fonts/noto/NotoSansArabic-Regular.ttf", -1},
    /* lang=ur (probe: heh-goal) — lang=ar can resolve to DejaVu, which lacks
     * the Urdu-only letters (U+06C1/U+06D2); Urdu coverage implies Arabic.
     * Kept eager: this shell's user writes Urdu (memory
     * `language-urdu-in-english-out`) — never tofu on frame 1. */
    {":lang=zh-cn", 0x4F60, "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", LAZY_CJK},
    {":lang=ja", 0x3053, "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", LAZY_CJK},
    {":lang=ko", 0xC548, "/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc", LAZY_CJK},
    {":lang=hi", 0x0928, NULL, LAZY_DEVANAGARI},
    {":lang=th", 0x0E2A, NULL, LAZY_THAI},
    {":lang=ta", 0x0B85, NULL, LAZY_TAMIL}, /* probe: TAMIL LETTER A */
};

/* Vendored monochrome emoji font, appended after the fontconfig chain. */
static const char *const EMOJI_FONT_CANDIDATES[] = {
    "assets/fonts/NotoEmoji-Regular.ttf",
    "/usr/share/dankc/fonts/NotoEmoji-Regular.ttf",
};

#define DC_MAX_FALLBACK_FONTS DC_RENDER_MAX_FALLBACKS

/* Dedupe list of every fallback font path accepted so far (zh-cn/ja/ko all
 * resolve to the same CJK .ttc): process-lifetime, shared between the
 * startup eager load and every later lazy load, so add_fallback_font()'s
 * dedupe check keeps working across the two call sites. */
static char g_loaded_paths[DC_MAX_FALLBACK_FONTS][256];
static int g_n_loaded_paths = 0;

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

/* docs/15-PERF-PLAN.md T1.1: mmap `path` read-only/private instead of the
 * fread-into-malloc that fontstash's own nvgCreateFont()/fonsAddFont() do.
 * Read-only MAP_PRIVATE pages are never written, so they're never promoted
 * to Private_Dirty — they stay file-backed (Shared_Clean/Private_Clean in
 * smaps) and the kernel only faults in the glyph-table pages a font actually
 * uses, evicting them under memory pressure like any other page-cache page.
 * The mapping is handed to nvgCreateFontMemAtIndex(..., freeData=0, ...),
 * which makes fontstash/stb_truetype retain the raw pointer directly (no
 * internal copy — see fonsAddFontMem() in third_party/nanovg/fontstash.h)
 * for as long as the NVGcontext lives, i.e. the whole process lifetime.
 * Deliberately never munmap'd once nanovg holds it: process exit reclaims
 * it, and unmapping under a live font atlas would be a use-after-unmap.
 * Returns NULL (leaving *out_size untouched) on any I/O failure. */
static unsigned char *mmap_font_file(const char *path, size_t *out_size)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return NULL;
    struct stat st;
    if (fstat(fd, &st) != 0 || st.st_size <= 0) {
        close(fd);
        return NULL;
    }
    void *data = mmap(NULL, (size_t)st.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); /* the mapping stays valid after the fd is closed */
    if (data == MAP_FAILED)
        return NULL;
    *out_size = (size_t)st.st_size;
    return (unsigned char *)data;
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

    size_t fsize = 0;
    unsigned char *fdata = mmap_font_file(path, &fsize);
    if (!fdata) {
        dc_warn("could not mmap fallback font %s", path);
        free(cov);
        return false;
    }
    /* fontIndex 0: matches the previous nvgCreateFont() behaviour — the CJK
     * .ttc collections here are always read at collection index 0 (see the
     * coverage block comment above load_cmap()). name just needs uniqueness. */
    int fb = nvgCreateFontMemAtIndex(render->vg, path, fdata, (int)fsize, 0, 0);
    if (fb < 0 || !nvgAddFallbackFontId(render->vg, render->font_ui, fb)) {
        dc_warn("could not load fallback font %s", path);
        munmap(fdata, fsize); /* fontstash never retained it (freeData=0, load failed) */
        free(cov);
        return false;
    }

    cov_merge(cov);
    /* Retained (not freed): render/shape.c's dc_render_font_for_codepoint()
     * needs per-font coverage, not just the merged union, to pick which
     * fallback font to hand HarfBuzz for a given run. */
    int idx = render->font_fallback_count;
    render->font_fallback_cov[idx] = cov;
    snprintf(render->font_fallback_paths[idx], sizeof(render->font_fallback_paths[idx]), "%s", path);
    render->font_fallbacks[render->font_fallback_count++] = fb;
    snprintf(loaded[(*n_loaded)++], 256, "%s", path);
    dc_info("fallback font %d: %s", render->font_fallback_count, path);
    return true;
}

/* Load only the EAGER fallback chain at startup: general Sans
 * (Cyrillic/Greek) + Arabic/Urdu (fallback_spec.lazy_group == -1). CJK,
 * Devanagari, Thai and emoji are deferred to service_lazy_fallbacks() (see
 * dc_render_note_codepoint()) — docs/16-PERF2-PLAN.md T2.1. Every step here
 * is still best-effort: a missing font just leaves that script uncovered,
 * and bar_sanitize_utf8() collapses it to "…" instead of tofu. */
static void load_fallback_fonts(dc_render *render)
{
    if (!FcInit()) {
        dc_warn("fontconfig init failed; no non-Latin fallback fonts");
        return;
    }
    for (size_t i = 0; i < sizeof(FALLBACK_SPECS) / sizeof(FALLBACK_SPECS[0]); i++) {
        const fallback_spec *spec = &FALLBACK_SPECS[i];
        if (spec->lazy_group != -1)
            continue;
        char *path = fc_match_path(spec->pattern);
        bool ok = path && add_fallback_font(render, path, spec->probe, g_loaded_paths, &g_n_loaded_paths);
        free(path);
        if (!ok && spec->retry_path && access(spec->retry_path, R_OK) == 0)
            add_fallback_font(render, spec->retry_path, spec->probe, g_loaded_paths, &g_n_loaded_paths);
    }
    FcFini();

    if (render->font_fallback_count == 0)
        dc_warn("no eager fallback fonts loaded; Latin-adjacent scripts will show tofu until"
                " a lazy fallback loads (see service_lazy_fallbacks)");
}

/* Load exactly the fallback font(s) for one LAZY_* group (CJK/Devanagari/
 * Thai via fontconfig, or the vendored emoji font), same best-effort rules
 * as load_fallback_fonts() above. Caller (service_lazy_fallbacks()) brackets
 * fontconfig-using groups with FcInit()/FcFini(); the emoji group needs
 * neither. */
static void load_one_lazy_group(dc_render *render, int group)
{
    if (group == LAZY_EMOJI) {
        for (size_t i = 0; i < sizeof(EMOJI_FONT_CANDIDATES) / sizeof(char *); i++) {
            if (access(EMOJI_FONT_CANDIDATES[i], R_OK) != 0)
                continue;
            if (add_fallback_font(render, EMOJI_FONT_CANDIDATES[i], 0x1F600, g_loaded_paths,
                                  &g_n_loaded_paths))
                break;
        }
        return;
    }

    for (size_t i = 0; i < sizeof(FALLBACK_SPECS) / sizeof(FALLBACK_SPECS[0]); i++) {
        const fallback_spec *spec = &FALLBACK_SPECS[i];
        if (spec->lazy_group != group)
            continue;
        char *path = fc_match_path(spec->pattern);
        bool ok = path && add_fallback_font(render, path, spec->probe, g_loaded_paths, &g_n_loaded_paths);
        free(path);
        if (!ok && spec->retry_path && access(spec->retry_path, R_OK) == 0)
            add_fallback_font(render, spec->retry_path, spec->probe, g_loaded_paths, &g_n_loaded_paths);
    }
}

/* Service every lazy-load request flagged by dc_render_note_codepoint()
 * since the last call. MUST only be called from dc_render_ensure(), which
 * every render path in this codebase (bar.c + every popout) calls strictly
 * before its nvgBeginFrame() — never mid-frame, so adding fonts to the
 * shared NVGcontext here is exactly as safe as the original startup-time
 * load_fallback_fonts() call was. If any font was actually added, resets
 * render/shape.c's shaped-text cache (dc_shape_reset_cache()): otherwise a
 * string shaped and cached BEFORE the new font existed (e.g. a CJK run that
 * fell back to font_ui and cached its .notdef glyph ids) would keep
 * rendering stale/wrong glyphs forever even after the real font loads,
 * since the cache key (font_ui id + size + text bytes) doesn't change. */
static void service_lazy_fallbacks(dc_render *render)
{
    unsigned todo = g_lazy_pending & ~g_lazy_loaded;
    if (!todo)
        return;

    int before = render->font_fallback_count;
    bool fc_ok = FcInit();
    for (int g = 0; g < LAZY_GROUP_COUNT; g++) {
        if (!(todo & (1u << g)))
            continue;
        if (g == LAZY_EMOJI || fc_ok)
            load_one_lazy_group(render, g);
        g_lazy_loaded |= (1u << g);
        dc_info("lazily loaded fallback font group %d (docs/16-PERF2-PLAN.md T2.1)", g);
    }
    if (fc_ok)
        FcFini();
    g_lazy_pending &= ~todo;

    if (render->font_fallback_count != before)
        dc_shape_reset_cache();
}

static int load_font(NVGcontext *vg, const char *name, const char *const *candidates, size_t count,
                     const char **out_path)
{
    for (size_t i = 0; i < count; i++) {
        if (access(candidates[i], R_OK) != 0)
            continue;
        size_t size = 0;
        unsigned char *data = mmap_font_file(candidates[i], &size);
        if (!data)
            continue;
        int font = nvgCreateFontMemAtIndex(vg, name, data, (int)size, 0, 0);
        if (font >= 0) {
            dc_debug("loaded font '%s': %s (mmap %zu bytes)", name, candidates[i], size);
            if (out_path)
                *out_path = candidates[i];
            return font;
        }
        munmap(data, size); /* fontstash never retained it (freeData=0, load failed) */
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
    if (render->ready && render->vg) {
        /* docs/16-PERF2-PLAN.md T2.1: service any lazy fallback-font load
         * requests flagged since the last frame. Always strictly before
         * this call's caller's nvgBeginFrame() — see service_lazy_fallbacks()
         * and dc_render_note_codepoint()'s doc comments. */
        service_lazy_fallbacks(render);
        return true;
    }
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
    render->font_ui_path[0] = '\0';
    render->font_ui_cov = NULL;
    if (ui_path) {
        font_coverage *cov = malloc(sizeof(*cov));
        if (cov) {
            load_cmap(ui_path, cov);
            if (cov->valid) {
                cov_merge(cov);
                g_cov.ui_valid = true;
                /* Retained (not freed) for dc_render_font_for_codepoint() —
                 * see the matching comment in add_fallback_font(). */
                render->font_ui_cov = cov;
                snprintf(render->font_ui_path, sizeof(render->font_ui_path), "%s", ui_path);
            } else {
                free(cov);
            }
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
    memset(render->font_fallback_cov, 0, sizeof(render->font_fallback_cov));
    load_fallback_fonts(render);

    render->ready = true;
    dc_debug("nanovg render context ready");
    return true;
}

bool dc_render_font_has(uint32_t codepoint)
{
    dc_render_note_codepoint(codepoint);
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

    /* Save/restore the full nvg state (font face, size, fill, align): leaking
     * the icon font into the caller's subsequent nvgText/nvgTextBounds calls
     * has caused garbled-text bugs in three separate widgets (media label,
     * notif-center Clear button, launcher footer). Never again. */
    nvgSave(render->vg);
    nvgFontFaceId(render->vg, render->font_icons);
    nvgFontSize(render->vg, size);
    nvgFillColor(render->vg, nvgRGBA(color.r, color.g, color.b, color.a));

    float draw_x = x;
    float draw_y = y;

    if (align_nvg & NVG_ALIGN_MIDDLE) {
        /* NVG_ALIGN_MIDDLE centers on the font's (ascender+descender)/2,
         * which is a text-layout metric, not the glyph's actual ink. For
         * the Material Symbols icon font that metrics box sits well above
         * a glyph's visual center, so every circular icon button (media
         * play/pause, etc.) rendered its glyph noticeably high. Measure
         * this glyph's real ink bounding box via nvgTextInkBounds() instead
         * and center that. (nvgTextBounds() computes the same ink box
         * internally but then discards it in favor of the font's line-box
         * metrics for y — a no-op for our purposes; see nanovg.c.) */
        int halign = align_nvg & (NVG_ALIGN_LEFT | NVG_ALIGN_CENTER | NVG_ALIGN_RIGHT);
        if (!halign)
            halign = NVG_ALIGN_LEFT;
        nvgTextAlign(render->vg, NVG_ALIGN_LEFT | NVG_ALIGN_BASELINE);
        float bounds[4];
        nvgTextInkBounds(render->vg, 0.0f, 0.0f, glyph, NULL, bounds);
        draw_y = y - (bounds[1] + bounds[3]) / 2.0f;
        nvgTextAlign(render->vg, halign | NVG_ALIGN_BASELINE);
    } else {
        nvgTextAlign(render->vg, align_nvg);
    }

    /* A play-triangle's ink centroid sits left of its bounding-box center
     * (it's wider on the left, where its two acute corners are). Bounding-
     * box centering alone therefore reads as very slightly left-heavy AND,
     * because the pointed apex draws the eye upward, very slightly high;
     * nudge right/down proportionally to the rendered size (not a flat
     * pixel) so it stays optically balanced across the bar's 14px, CC's
     * 16px, and the dashboard's ~19-25px transport glyph alike. Tuned by
     * eye against zoomed screenshots at all three sizes. */
    if (codepoint == DC_ICON_PLAY_ARROW) {
        draw_x += size * 0.07f;
        draw_y += size * 0.07f;
    }

    nvgText(render->vg, draw_x, draw_y, glyph, NULL);
    nvgRestore(render->vg);
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
    free(render->font_ui_cov);
    render->font_ui_cov = NULL;
    for (int i = 0; i < render->font_fallback_count; i++) {
        free(render->font_fallback_cov[i]);
        render->font_fallback_cov[i] = NULL;
    }
    render->font_fallback_count = 0;
    memset(&g_cov, 0, sizeof(g_cov));

    /* Reset the lazy-loading bookkeeping too: a subsequent dc_render_ensure()
     * rebuilds a brand-new NVGcontext with no fonts registered, so the
     * dedupe list (g_loaded_paths) and per-group loaded/pending bitmasks must
     * not carry over — otherwise add_fallback_font()'s dedupe check would
     * see stale "already loaded" paths and skip re-registering them on the
     * new context (docs/16-PERF2-PLAN.md T2.1). */
    g_n_loaded_paths = 0;
    g_lazy_loaded = 0;
    g_lazy_pending = 0;
}

int dc_render_font_for_codepoint(dc_render *render, uint32_t codepoint, const char **out_path)
{
    dc_render_note_codepoint(codepoint);
    if (render->font_ui_cov && cov_get(render->font_ui_cov, codepoint)) {
        if (out_path)
            *out_path = render->font_ui_path;
        return render->font_ui;
    }
    for (int i = 0; i < render->font_fallback_count; i++) {
        if (render->font_fallback_cov[i] && cov_get(render->font_fallback_cov[i], codepoint)) {
            if (out_path)
                *out_path = render->font_fallback_paths[i];
            return render->font_fallbacks[i];
        }
    }
    return -1;
}
