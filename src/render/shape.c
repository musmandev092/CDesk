#include "render/shape.h"

#include "core/log.h"
#include "nanovg.h"

#include <hb-ot.h>
#include <hb.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef DC_HAVE_FRIBIDI
#include <fribidi/fribidi.h>
#endif

/* --- script detection (docs task spec: Arabic + Hebrew ranges) ------------ */

static bool cp_needs_shaping(uint32_t cp)
{
    return (cp >= 0x0600 && cp <= 0x06FF) || (cp >= 0x0750 && cp <= 0x077F) ||
           (cp >= 0x08A0 && cp <= 0x08FF) || (cp >= 0xFB50 && cp <= 0xFDFF) ||
           (cp >= 0xFE70 && cp <= 0xFEFF) || (cp >= 0x0590 && cp <= 0x05FF);
}

static uint32_t utf8_decode(const unsigned char *s, const unsigned char *end, int *out_len)
{
    unsigned char c = *s;
    int len;
    uint32_t cp;

    if ((c & 0x80) == 0) {
        len = 1;
        cp = c;
    } else if ((c & 0xE0) == 0xC0) {
        len = 2;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        len = 3;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        len = 4;
        cp = c & 0x07;
    } else {
        *out_len = 1;
        return 0xFFFD;
    }
    if (s + len > end) {
        *out_len = 1;
        return 0xFFFD;
    }
    for (int i = 1; i < len; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *out_len = 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *out_len = len;
    return cp;
}

/* docs/16-PERF2-PLAN.md T2.1: this is the one scan every title/toast/
 * notification string in the shell passes through before drawing (all of
 * dc_shape_draw_text/dc_shape_text_bounds/dc_shape_draw_textbox gate on it
 * first) — so it doubles as the hook for lazy fallback-font loading: every
 * codepoint is reported to dc_render_note_codepoint() (nvg.c), which flags a
 * not-yet-loaded script (CJK/Devanagari/Thai/emoji) for loading before the
 * next frame AND returns true if that codepoint's fallback isn't loaded yet.
 * That return value MUST force `needed = true` here (not just
 * cp_needs_shaping()'s Arabic/Hebrew check): while a lazy fallback is still
 * loading, its codepoints must go through the shaped nvgTextGlyphs()
 * (glyph-index) draw path rather than plain nvgText()'s codepoint-keyed
 * path, because fontstash's fons__getGlyph() permanently caches a "no glyph"
 * result per (font, codepoint, size) the first time it's asked, and
 * registering the real fallback font later can never retroactively fix an
 * already-cached miss (see nvg.h's dc_render_note_codepoint() doc comment).
 * This also means the scan can no longer return early on the first
 * shaping-needed codepoint (it needs to see the whole string to flag
 * everything), but the string lengths here (titles/toasts) are short enough
 * that this is not a measurable cost — same O(n) single pass as before. */
bool dc_shape_needed(const char *text, const char *end)
{
    if (!text)
        return false;
    const unsigned char *s = (const unsigned char *)text;
    const unsigned char *e = end ? (const unsigned char *)end : s + strlen(text);
    bool needed = false;
    while (s < e) {
        int len;
        uint32_t cp = utf8_decode(s, e, &len);
        if (dc_render_note_codepoint(cp))
            needed = true;
        if (cp_needs_shaping(cp))
            needed = true;
        s += len;
    }
    return needed;
}

/* --- HarfBuzz font cache (keyed by font file path) ------------------------
 *
 * hb_font_t built from the exact same font file bytes nvg's stb_truetype
 * loaded (via dc_render_font_for_codepoint()'s path), using HarfBuzz's own
 * OpenType backend (hb_ot_font_set_funcs) — no FreeType dependency needed
 * for shaping. Glyph indices HarfBuzz produces this way are indices into
 * that font FILE's glyf table, which is exactly what fontstash's
 * stb_truetype parse of the same file also indexes by, so a glyph id from
 * this hb_font_t is valid to feed straight to fonsGetGlyphQuadByIndex()
 * against the matching nvg font id. */
#define HB_FONT_CACHE_MAX 8

typedef struct {
    char path[256];
    hb_font_t *font;
} hb_font_cache_entry;

static hb_font_cache_entry g_hb_fonts[HB_FONT_CACHE_MAX];
static int g_hb_font_count = 0;

static hb_font_t *hb_font_for_path(const char *path)
{
    for (int i = 0; i < g_hb_font_count; i++)
        if (strcmp(g_hb_fonts[i].path, path) == 0)
            return g_hb_fonts[i].font;

    hb_blob_t *blob = hb_blob_create_from_file(path);
    if (!blob || hb_blob_get_length(blob) == 0) {
        dc_warn("shape: could not read font %s for HarfBuzz", path);
        if (blob)
            hb_blob_destroy(blob);
        return NULL;
    }
    hb_face_t *face = hb_face_create(blob, 0);
    hb_blob_destroy(blob);
    hb_font_t *font = hb_font_create(face);
    hb_face_destroy(face);
    hb_ot_font_set_funcs(font);
    int upem = (int)hb_face_get_upem(hb_font_get_face(font));
    if (upem <= 0)
        upem = 1000;
    hb_font_set_scale(font, upem, upem);

    if (g_hb_font_count < HB_FONT_CACHE_MAX) {
        snprintf(g_hb_fonts[g_hb_font_count].path, sizeof(g_hb_fonts[g_hb_font_count].path), "%s", path);
        g_hb_fonts[g_hb_font_count].font = font;
        g_hb_font_count++;
    } else {
        /* Cache exhausted (shouldn't happen in practice: dankc only ever
         * shapes against font_ui plus whichever single fallback font covers
         * Arabic/Hebrew). Return the font unregistered rather than fail the
         * draw; it's a small one-time leak of a process-lifetime object. */
        dc_warn("shape: HarfBuzz font cache full, not caching %s", path);
    }
    return font;
}

/* --- BiDi: logical text -> ordered (visual left-to-right) direction runs -- */

typedef struct {
    int start_cp, end_cp; /* [start,end) codepoint index range, LOGICAL order */
    bool rtl;
} bidi_run;

#define MAX_BIDI_RUNS 64

#ifdef DC_HAVE_FRIBIDI

/* Real UAX#9 BiDi via FriBidi: resolve embedding levels, then reorder an
 * identity index map from logical to visual order (fribidi_reorder_line).
 * Walking the reordered map finds maximal runs whose logical indices are
 * contiguous and monotonic (increasing for an LTR sub-run, decreasing for
 * RTL) with constant level parity — the standard way to recover per-run
 * direction + logical span from FriBidi's flat visual reordering, letting
 * each run be shaped independently (HarfBuzz needs one direction/script per
 * buffer) while still emitting runs in true visual left-to-right order. */
static int bidi_compute_runs(const uint32_t *cps, int n, bidi_run *runs, int max_runs, bool *out_base_rtl)
{
    if (n <= 0)
        return 0;

    FriBidiChar *fbtext = malloc(sizeof(FriBidiChar) * (size_t)n);
    FriBidiCharType *btypes = malloc(sizeof(FriBidiCharType) * (size_t)n);
    FriBidiBracketType *brackets = malloc(sizeof(FriBidiBracketType) * (size_t)n);
    FriBidiLevel *levels = malloc(sizeof(FriBidiLevel) * (size_t)n);
    FriBidiStrIndex *vis2log = malloc(sizeof(FriBidiStrIndex) * (size_t)n);
    int nruns = 0;

    if (!fbtext || !btypes || !brackets || !levels || !vis2log)
        goto out;

    for (int i = 0; i < n; i++)
        fbtext[i] = (FriBidiChar)cps[i];

    fribidi_get_bidi_types(fbtext, n, btypes);
    fribidi_get_bracket_types(fbtext, n, btypes, brackets);

    FriBidiParType base_dir = FRIBIDI_PAR_ON; /* auto-detect from content */
    FriBidiLevel max_level = fribidi_get_par_embedding_levels_ex(btypes, brackets, n, &base_dir, levels);
    if (max_level == 0) /* allocation failure inside FriBidi */
        goto out;
    *out_base_rtl = FRIBIDI_IS_RTL(base_dir) != 0;

    for (int i = 0; i < n; i++)
        vis2log[i] = i;
    if (fribidi_reorder_line(FRIBIDI_FLAGS_DEFAULT, btypes, n, 0, base_dir, levels, NULL, vis2log) == 0)
        goto out;

    int vi = 0;
    while (vi < n && nruns < max_runs) {
        int lstart = vis2log[vi];
        bool rtl = FRIBIDI_LEVEL_IS_RTL(levels[lstart]) != 0;
        int lprev = lstart;
        int vj = vi + 1;
        while (vj < n) {
            int lcur = vis2log[vj];
            bool cur_rtl = FRIBIDI_LEVEL_IS_RTL(levels[lcur]) != 0;
            if (cur_rtl != rtl)
                break;
            if (rtl ? (lcur != lprev - 1) : (lcur != lprev + 1))
                break;
            lprev = lcur;
            vj++;
        }
        int lo = rtl ? lprev : lstart;
        int hi = rtl ? lstart : lprev;
        runs[nruns].start_cp = lo;
        runs[nruns].end_cp = hi + 1;
        runs[nruns].rtl = rtl;
        nruns++;
        vi = vj;
    }

out:
    free(fbtext);
    free(btypes);
    free(brackets);
    free(levels);
    free(vis2log);
    return nruns;
}

#else /* !DC_HAVE_FRIBIDI */

/* UBA-lite fallback used only when FriBidi isn't available at build time
 * (see meson.build / Makefile — gated behind pkg-config finding fribidi).
 * Simplified two-level model, exactly per docs/POLISH.md's fallback spec:
 * split the logical string into maximal contiguous runs of RTL-range vs.
 * everything-else codepoints (so an embedded run of ASCII digits inside
 * Arabic/Urdu text stays its own, LTR-ordered run rather than getting
 * swept into the surrounding RTL reversal), then — if the paragraph is
 * predominantly RTL (its first run is RTL) — reverse the overall RUN order
 * so runs are emitted left-to-right visually. This is NOT full UAX#9 (no
 * paragraph-relative neutral resolution, no nested embedding), but covers
 * the common case of an RTL sentence with embedded Latin brand names or
 * numbers. */
/* Every script we detect here is also RTL, so run-direction and
 * needs-shaping are decided by the same ranges. Kept as a separate name at
 * this (fallback-only) call site for clarity, not because the logic
 * differs. */
static bool cp_is_rtl_range(uint32_t cp)
{
    return cp_needs_shaping(cp);
}

static int bidi_compute_runs(const uint32_t *cps, int n, bidi_run *runs, int max_runs, bool *out_base_rtl)
{
    int nruns = 0;
    int i = 0;
    bool first = true;
    *out_base_rtl = false;

    while (i < n && nruns < max_runs) {
        bool rtl = cp_is_rtl_range(cps[i]);
        int j = i + 1;
        while (j < n && cp_is_rtl_range(cps[j]) == rtl)
            j++;
        if (first) {
            *out_base_rtl = rtl;
            first = false;
        }
        runs[nruns].start_cp = i;
        runs[nruns].end_cp = j;
        runs[nruns].rtl = rtl;
        nruns++;
        i = j;
    }

    if (*out_base_rtl) {
        for (int a = 0, b = nruns - 1; a < b; a++, b--) {
            bidi_run t = runs[a];
            runs[a] = runs[b];
            runs[b] = t;
        }
    }
    return nruns;
}

#endif /* DC_HAVE_FRIBIDI */

/* --- shaped-text cache (small LRU, keyed on font id + size + text) -------- */

typedef struct {
    int gid;
    int font_id; /* nvg font id this glyph must be drawn with */
    float dx, dy;
    float adv; /* visual x-advance contributed, for measurement/ellipsis */
} dc_shape_glyph;

#define SHAPE_CACHE_MAX 48

typedef struct shape_entry {
    char *key;
    dc_shape_glyph *glyphs;
    int nglyphs;
    float width;
    bool base_rtl;
    struct shape_entry *prev, *next;
} shape_entry;

static shape_entry *g_cache[SHAPE_CACHE_MAX];
static shape_entry *g_lru_head = NULL; /* most recently used */
static shape_entry *g_lru_tail = NULL; /* least recently used */
static int g_cache_count = 0;

static void lru_unlink(shape_entry *e)
{
    if (e->prev)
        e->prev->next = e->next;
    else
        g_lru_head = e->next;
    if (e->next)
        e->next->prev = e->prev;
    else
        g_lru_tail = e->prev;
    e->prev = e->next = NULL;
}

static void lru_push_front(shape_entry *e)
{
    e->prev = NULL;
    e->next = g_lru_head;
    if (g_lru_head)
        g_lru_head->prev = e;
    g_lru_head = e;
    if (!g_lru_tail)
        g_lru_tail = e;
}

static void shape_entry_free(shape_entry *e)
{
    free(e->key);
    free(e->glyphs);
    free(e);
}

static shape_entry *cache_find(const char *key)
{
    for (int i = 0; i < g_cache_count; i++) {
        if (strcmp(g_cache[i]->key, key) == 0) {
            shape_entry *e = g_cache[i];
            lru_unlink(e);
            lru_push_front(e);
            return e;
        }
    }
    return NULL;
}

static void cache_insert(shape_entry *e)
{
    if (g_cache_count >= SHAPE_CACHE_MAX) {
        /* Evict least-recently-used. */
        shape_entry *victim = g_lru_tail;
        lru_unlink(victim);
        for (int i = 0; i < g_cache_count; i++) {
            if (g_cache[i] == victim) {
                g_cache[i] = g_cache[--g_cache_count];
                break;
            }
        }
        shape_entry_free(victim);
    }
    g_cache[g_cache_count++] = e;
    lru_push_front(e);
}

void dc_shape_reset_cache(void)
{
    for (int i = 0; i < g_cache_count; i++)
        shape_entry_free(g_cache[i]);
    g_cache_count = 0;
    g_lru_head = g_lru_tail = NULL;

    for (int i = 0; i < g_hb_font_count; i++)
        hb_font_destroy(g_hb_fonts[i].font);
    g_hb_font_count = 0;
}

/* --- shaping: logical UTF-8 text -> cached shape_entry --------------------
 *
 * Decodes to codepoints, runs BiDi to get visual-order direction runs, then
 * HarfBuzz-shapes each run against whichever loaded font covers its first
 * meaningful codepoint (dc_render_font_for_codepoint() — LTR runs
 * overwhelmingly resolve to font_ui itself, exactly matching what an
 * unshaped nvgText() draw of the same substring would have used). Glyphs
 * from all runs are appended in visual left-to-right order with a single
 * running pen-x, so the result can be drawn as one flat sequence. */
static shape_entry *shape_build(dc_render *render, const char *text, const char *end, float font_size)
{
    const unsigned char *s = (const unsigned char *)text;
    const unsigned char *e = end ? (const unsigned char *)end : s + strlen(text);
    size_t byte_len = (size_t)(e - s);

    /* Decode to codepoints (bounded — titles/toasts/entries are short; a
     * pathological huge string just gets shaped up to the cap, which only
     * affects rendering of text far wider than any dankc widget anyway). */
    enum { MAX_CHARS = 512 };
    uint32_t cps[MAX_CHARS];
    int byte_off[MAX_CHARS + 1];
    int n = 0;
    {
        const unsigned char *p = s;
        while (p < e && n < MAX_CHARS) {
            int len;
            byte_off[n] = (int)(p - s);
            cps[n] = utf8_decode(p, e, &len);
            p += len;
            n++;
        }
        byte_off[n] = (int)(p - s);
    }
    if (n == 0)
        return NULL;

    bidi_run runs[MAX_BIDI_RUNS];
    bool base_rtl = false;
    int nruns = bidi_compute_runs(cps, n, runs, MAX_BIDI_RUNS, &base_rtl);
    if (nruns == 0)
        return NULL;

    dc_shape_glyph *glyphs = NULL;
    int nglyphs = 0, cap = 0;
    float pen_x = 0.0f;

    for (int r = 0; r < nruns; r++) {
        int rstart = runs[r].start_cp, rend = runs[r].end_cp;
        if (rstart >= rend)
            continue;

        /* Font for this run: first font (font_ui, then fallbacks, in
         * registered priority order) that actually covers the run's first
         * non-whitespace codepoint — matching fontstash's own resolution
         * order for an unshaped draw of the same text. */
        int font_id = -1;
        const char *font_path = NULL;
        for (int i = rstart; i < rend && font_id < 0; i++) {
            if (cps[i] <= 0x20)
                continue;
            font_id = dc_render_font_for_codepoint(render, cps[i], &font_path);
        }
        if (font_id < 0) {
            font_id = render->font_ui;
            font_path = render->font_ui_path;
        }
        if (!font_path || !font_path[0])
            continue;

        hb_font_t *hbfont = hb_font_for_path(font_path);
        if (!hbfont)
            continue;

        int byte_start = byte_off[rstart];
        int byte_end = byte_off[rend];

        hb_buffer_t *buf = hb_buffer_create();
        hb_buffer_add_utf8(buf, text, (int)byte_len, (unsigned int)byte_start, byte_end - byte_start);
        hb_buffer_set_direction(buf, runs[r].rtl ? HB_DIRECTION_RTL : HB_DIRECTION_LTR);
        hb_buffer_guess_segment_properties(buf);
        hb_shape(hbfont, buf, NULL, 0);

        unsigned int raw_n = 0;
        hb_glyph_info_t *info = hb_buffer_get_glyph_infos(buf, &raw_n);
        hb_glyph_position_t *pos = hb_buffer_get_glyph_positions(buf, &raw_n);
        int ninfo = (int)raw_n;
        int upem = (int)hb_face_get_upem(hb_font_get_face(hbfont));
        if (upem <= 0)
            upem = 1000;
        float unit_scale = font_size / (float)upem;

        if (nglyphs + ninfo > cap) {
            cap = (nglyphs + ninfo) * 2 + 8;
            dc_shape_glyph *ng = realloc(glyphs, sizeof(*ng) * (size_t)cap);
            if (!ng) {
                hb_buffer_destroy(buf);
                free(glyphs);
                return NULL;
            }
            glyphs = ng;
        }
        for (int i = 0; i < ninfo; i++) {
            glyphs[nglyphs].gid = (int)info[i].codepoint;
            glyphs[nglyphs].font_id = font_id;
            glyphs[nglyphs].dx = pen_x + (float)pos[i].x_offset * unit_scale;
            glyphs[nglyphs].dy = -(float)pos[i].y_offset * unit_scale; /* HB y-up, nvg y-down */
            glyphs[nglyphs].adv = (float)pos[i].x_advance * unit_scale;
            pen_x += glyphs[nglyphs].adv;
            nglyphs++;
        }
        hb_buffer_destroy(buf);
    }

    shape_entry *ent = calloc(1, sizeof(*ent));
    if (!ent) {
        free(glyphs);
        return NULL;
    }
    ent->glyphs = glyphs;
    ent->nglyphs = nglyphs;
    ent->width = pen_x;
    ent->base_rtl = base_rtl;
    return ent;
}

static shape_entry *shape_get(dc_render *render, const char *text, const char *end, float font_size)
{
    size_t tlen = end ? (size_t)(end - text) : strlen(text);
    /* Key = font_ui id (invalidated wholesale by dc_shape_reset_cache() on
     * any font/theme reload, so a stale id can never alias a live one) +
     * size (quantized to 0.1px) + the text bytes. */
    size_t keycap = tlen + 48;
    char *key = malloc(keycap);
    if (!key)
        return NULL;
    int prefix = snprintf(key, keycap, "%d|%ld|", render->font_ui, (long)(font_size * 10.0f + 0.5f));
    if (prefix < 0)
        prefix = 0;
    size_t room = (size_t)prefix < keycap - 1 ? keycap - 1 - (size_t)prefix : 0;
    size_t copy_n = tlen < room ? tlen : room;
    memcpy(key + prefix, text, copy_n);
    key[prefix + (int)copy_n] = '\0';

    shape_entry *hit = cache_find(key);
    if (hit) {
        free(key);
        return hit;
    }

    shape_entry *ent = shape_build(render, text, end, font_size);
    if (!ent) {
        free(key);
        return NULL;
    }
    ent->key = key; /* ownership transferred */
    cache_insert(ent);
    return ent;
}

/* --- public drop-in entry points ------------------------------------------ */

float dc_shape_draw_text(dc_render *render, float x, float y, const char *text, const char *end)
{
    NVGcontext *vg = render->vg;
    if (!dc_shape_needed(text, end))
        return nvgText(vg, x, y, text, end);

    float size = nvgCurrentFontSize(vg);
    shape_entry *ent = shape_get(render, text, end, size);
    if (!ent || ent->nglyphs == 0)
        return nvgText(vg, x, y, text, end);

    int align = nvgCurrentTextAlign(vg);
    int saved_font = nvgCurrentFontId(vg);
    float total_w = ent->width;
    float x0 = x;
    if (align & NVG_ALIGN_CENTER)
        x0 = x - total_w * 0.5f;
    else if (align & NVG_ALIGN_RIGHT)
        x0 = x - total_w;

    /* nvgTextGlyphs() only honors vertical align (see nanovg.h); horizontal
     * align is resolved above by shifting the origin, so force LEFT for the
     * actual glyph draw while leaving the vertical bits untouched. */
    int valign_only = align & (NVG_ALIGN_TOP | NVG_ALIGN_MIDDLE | NVG_ALIGN_BOTTOM | NVG_ALIGN_BASELINE);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | (valign_only ? valign_only : NVG_ALIGN_BASELINE));

    int i = 0;
    NVGglyphRun run_buf[64];
    while (i < ent->nglyphs) {
        int font_id = ent->glyphs[i].font_id;
        int nrun = 0;
        while (i < ent->nglyphs && ent->glyphs[i].font_id == font_id && nrun < 64) {
            run_buf[nrun].gid = ent->glyphs[i].gid;
            run_buf[nrun].x = ent->glyphs[i].dx;
            run_buf[nrun].y = ent->glyphs[i].dy;
            nrun++;
            i++;
        }
        nvgFontFaceId(vg, font_id);
        nvgTextGlyphs(vg, x0, y, run_buf, nrun);
    }
    /* Always restore the face the caller had bound — a fallback face (e.g.
     * the Arabic font used for an RTL run above) must never leak into the
     * caller's next nvgText()/dc_shape_draw_text() call. See nvg.c's
     * dc_render_icon() comment for the class of bug this guards against. */
    nvgFontFaceId(vg, saved_font);
    nvgTextAlign(vg, align);

    return x0 + total_w;
}

float dc_shape_text_bounds(dc_render *render, float x, float y, const char *text, const char *end,
                           float *bounds)
{
    NVGcontext *vg = render->vg;
    if (!dc_shape_needed(text, end))
        return nvgTextBounds(vg, x, y, text, end, bounds);

    float asc = 0, desc = 0;
    nvgTextMetrics(vg, &asc, &desc, NULL);
    float size = nvgCurrentFontSize(vg);

    shape_entry *ent = shape_get(render, text, end, size);
    if (!ent) {
        if (bounds)
            bounds[0] = bounds[1] = bounds[2] = bounds[3] = x;
        return x;
    }

    if (bounds) {
        bounds[0] = x;
        bounds[1] = y - asc;
        bounds[2] = x + ent->width;
        bounds[3] = y - desc;
    }
    return x + ent->width;
}

void dc_shape_draw_textbox(dc_render *render, float x, float y, float break_row_width, const char *text,
                           const char *end)
{
    NVGcontext *vg = render->vg;
    if (!dc_shape_needed(text, end)) {
        nvgTextBox(vg, x, y, break_row_width, text, end);
        return;
    }

    NVGtextRow rows[3];
    float lineh = 0;
    nvgTextMetrics(vg, NULL, NULL, &lineh);
    int align = nvgCurrentTextAlign(vg);
    int halign = align & (NVG_ALIGN_LEFT | NVG_ALIGN_CENTER | NVG_ALIGN_RIGHT);
    int valign = align & (NVG_ALIGN_TOP | NVG_ALIGN_MIDDLE | NVG_ALIGN_BOTTOM | NVG_ALIGN_BASELINE);

    nvgTextAlign(vg, NVG_ALIGN_LEFT | valign);
    const char *p = text;
    const char *text_end = end ? end : text + strlen(text);
    float cy = y;
    int nrows;
    while (p < text_end && (nrows = nvgTextBreakLines(vg, p, end, break_row_width, rows, 3)) > 0) {
        for (int i = 0; i < nrows; i++) {
            NVGtextRow *row = &rows[i];
            float rx = x;
            if (halign & NVG_ALIGN_CENTER)
                rx = x + break_row_width * 0.5f - row->width * 0.5f;
            else if (halign & NVG_ALIGN_RIGHT)
                rx = x + break_row_width - row->width;
            dc_shape_draw_text(render, rx, cy, row->start, row->end);
            cy += lineh;
        }
        p = rows[nrows - 1].next;
    }
    nvgTextAlign(vg, align);
}

float dc_shape_ellipsize(dc_render *render, const char *text, float max_width, char *out, size_t out_sz)
{
    if (out_sz == 0)
        return 0.0f;

    float bounds[4];
    float full_w = dc_shape_text_bounds(render, 0.0f, 0.0f, text, NULL, bounds);
    if (bounds[2] - bounds[0] <= max_width) {
        snprintf(out, out_sz, "%s", text);
        return full_w;
    }

    /* Codepoint-boundary byte offsets, bounded like shape_build()'s own
     * per-string cap — a string longer than that already can't be shaped
     * exactly by dc_shape_text_bounds() either, so there's nothing more
     * precise to binary-search over past this point. */
    enum { MAX_CHARS = 512 };
    int offs[MAX_CHARS + 1];
    int ncp = 0;
    {
        const unsigned char *p = (const unsigned char *)text;
        const unsigned char *pe = p + strlen(text);
        while (p < pe && ncp < MAX_CHARS) {
            int len;
            offs[ncp] = (int)((const char *)p - text);
            (void)utf8_decode(p, pe, &len);
            p += len;
            ncp++;
        }
        offs[ncp] = (int)((const char *)p - text);
    }
    /* Leave room for the 3-byte ellipsis + NUL in `out`. */
    while (ncp > 0 && offs[ncp] + 4 > (int)out_sz)
        ncp--;

    char *tmp = malloc((size_t)offs[ncp] + 8);
    if (!tmp) {
        snprintf(out, out_sz, "\xe2\x80\xa6");
        return 0.0f;
    }

    /* Binary search (over codepoint count, not bytes) for the longest
     * logical prefix whose shaped width + ellipsis still fits — O(log n)
     * reshapes instead of the O(n) a byte-at-a-time shrink would cost,
     * which matters here since each trial is a fresh cached HarfBuzz shape
     * and a long RTL title would otherwise evict useful cache entries (the
     * final result included) on every overflowing frame. Width is
     * monotonically non-decreasing in prefix length for normal text, so the
     * search is valid. Truncating the *logical* string (not reasoning about
     * visual position) and appending the ellipsis at the logical end is
     * also what puts the ellipsis on the visually correct side for both
     * LTR and RTL paragraphs — see dc_shape_ellipsize()'s doc comment. */
    int lo = 0, hi = ncp, best = 0;
    float best_w = 0.0f;
    while (lo <= hi) {
        int mid = lo + (hi - lo) / 2;
        int blen = offs[mid];
        memcpy(tmp, text, (size_t)blen);
        memcpy(tmp + blen, "\xe2\x80\xa6", 3);
        tmp[blen + 3] = '\0';
        dc_shape_text_bounds(render, 0.0f, 0.0f, tmp, NULL, bounds);
        if (bounds[2] - bounds[0] <= max_width) {
            best = mid;
            best_w = bounds[2] - bounds[0];
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    int blen = offs[best];
    memcpy(tmp, text, (size_t)blen);
    memcpy(tmp + blen, "\xe2\x80\xa6", 3);
    tmp[blen + 3] = '\0';
    snprintf(out, out_sz, "%s", tmp);
    free(tmp);
    return best_w;
}
