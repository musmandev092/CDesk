#include "ui/text_edit.h"

#include "core/anim.h" /* dc_anim_now_ms() -- monotonic clock, reused rather than duplicated */
#include "core/log.h"
#include "dc.h"
#include "render/nvg.h"
#include "render/shape.h"
#include "theme/theme.h"

#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "nanovg.h"

/* docs/22-NOTEPAD-PLAN.md §2: 14px UI font, 1.4x line-height multiplier
 * (matching clip_picker.c's own convention of a plain fontSize*multiplier
 * line-advance rather than querying nvgTextMetrics' font-native lineh). */
#define TE_FONT_SIZE 14.0f
#define TE_LINE_HEIGHT_MULT 1.4f
#define TE_CURSOR_W 2.0f
#define TE_SCROLLBAR_W 3.0f
#define TE_SCROLL_STEP 24.0f /* px per wheel "step", matching clip_picker's DC_CP_SCROLL_STEP convention */

#define TE_BUF_INITIAL_CAP 256
#define TE_ROW_CAP_INITIAL 64
/* Per-row glyph-position scratch buffer size (stack-allocated): generous for
 * a single wrapped row at 14px in any panel width this shell uses; a row
 * with more glyphs than this just measures its excess via row->width instead
 * of per-glyph (only matters for extremely dense CJK rows far wider than any
 * planned Notepad panel). */
#define TE_GLYPH_SCRATCH 512

static inline float te_line_h(void)
{
    return TE_FONT_SIZE * TE_LINE_HEIGHT_MULT;
}

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

/* ========================================================================
 * Lifecycle
 * ======================================================================== */

void dc_text_edit_init(dc_text_edit *te)
{
    memset(te, 0, sizeof(*te));
    te->buf = calloc(1, TE_BUF_INITIAL_CAP);
    te->cap = te->buf ? TE_BUF_INITIAL_CAP : 0;
    te->preferred_x = -1.0f;
    te->layout_wrap_w = 1.0e6f; /* sane default if a key ever arrives before the first draw() */
    te->layout_valid = false;
}

void dc_text_edit_free(dc_text_edit *te)
{
    if (!te)
        return;
    free(te->buf);
    free(te->rows);
    memset(te, 0, sizeof(*te));
}

/* ========================================================================
 * Pure buffer ops -- no GL/EGL/nanovg touched anywhere below this point
 * until the "layout cache" section. Safe to unit test without a GL context.
 * ======================================================================== */

/* Grow-by-doubling to hold `need_len` content bytes (+1 for the NUL we
 * always keep after the live content), capped at DC_TEXT_EDIT_MAX_BYTES.
 * Refuses (dc_warn + false) rather than partially growing/truncating. */
static bool te_grow(dc_text_edit *te, size_t need_len)
{
    if (need_len > DC_TEXT_EDIT_MAX_BYTES) {
        dc_warn("text_edit: refusing edit, would grow to %zu bytes (cap %u)", need_len,
                (unsigned)DC_TEXT_EDIT_MAX_BYTES);
        return false;
    }
    if (need_len + 1 <= te->cap)
        return true;
    size_t newcap = te->cap ? te->cap : TE_BUF_INITIAL_CAP;
    while (newcap < need_len + 1)
        newcap *= 2;
    if (newcap > (size_t)DC_TEXT_EDIT_MAX_BYTES + 1)
        newcap = (size_t)DC_TEXT_EDIT_MAX_BYTES + 1;
    char *nb = realloc(te->buf, newcap);
    if (!nb) {
        dc_warn("text_edit: realloc failed growing buffer to %zu bytes", newcap);
        return false;
    }
    te->buf = nb;
    te->cap = newcap;
    return true;
}

size_t dc_text_edit_prev_boundary(const dc_text_edit *te, size_t pos)
{
    if (!te || pos == 0)
        return 0;
    if (pos > te->len)
        pos = te->len;
    pos--;
    while (pos > 0 && ((unsigned char)te->buf[pos] & 0xC0) == 0x80)
        pos--;
    return pos;
}

size_t dc_text_edit_next_boundary(const dc_text_edit *te, size_t pos)
{
    if (!te)
        return 0;
    if (pos >= te->len)
        return te->len;
    pos++;
    while (pos < te->len && ((unsigned char)te->buf[pos] & 0xC0) == 0x80)
        pos++;
    return pos;
}

void dc_text_edit_set_cursor(dc_text_edit *te, size_t pos)
{
    if (!te)
        return;
    if (pos > te->len)
        pos = te->len;
    while (pos > 0 && ((unsigned char)te->buf[pos] & 0xC0) == 0x80)
        pos--;
    te->cursor = pos;
    te->preferred_x = -1.0f;
}

static void te_touch_edited(dc_text_edit *te)
{
    te->layout_valid = false;
    te->dirty = true;
    te->last_edit_ms = (uint64_t)dc_anim_now_ms();
    te->preferred_x = -1.0f;
}

bool dc_text_edit_insert_utf8(dc_text_edit *te, const char *utf8, size_t n)
{
    if (!te || !utf8 || n == 0)
        return false;
    if (!te_grow(te, te->len + n))
        return false;
    if (te->cursor > te->len)
        te->cursor = te->len; /* defensive; should already be a valid boundary */
    memmove(te->buf + te->cursor + n, te->buf + te->cursor, te->len - te->cursor);
    memcpy(te->buf + te->cursor, utf8, n);
    te->len += n;
    te->cursor += n;
    te->buf[te->len] = '\0';
    te_touch_edited(te);
    return true;
}

void dc_text_edit_delete_backward(dc_text_edit *te)
{
    if (!te || te->cursor == 0)
        return;
    size_t prev = dc_text_edit_prev_boundary(te, te->cursor);
    size_t n = te->cursor - prev;
    memmove(te->buf + prev, te->buf + te->cursor, te->len - te->cursor);
    te->len -= n;
    te->buf[te->len] = '\0';
    te->cursor = prev;
    te_touch_edited(te);
}

void dc_text_edit_delete_forward(dc_text_edit *te)
{
    if (!te || te->cursor >= te->len)
        return;
    size_t next = dc_text_edit_next_boundary(te, te->cursor);
    size_t n = next - te->cursor;
    memmove(te->buf + te->cursor, te->buf + next, te->len - next);
    te->len -= n;
    te->buf[te->len] = '\0';
    te_touch_edited(te);
}

void dc_text_edit_set_text(dc_text_edit *te, const char *text, size_t len)
{
    if (!te)
        return;
    if (!text)
        len = 0;
    if (len > DC_TEXT_EDIT_MAX_BYTES) {
        size_t cut = DC_TEXT_EDIT_MAX_BYTES;
        /* Don't split a multi-byte codepoint at the cut point. */
        while (cut > 0 && ((unsigned char)text[cut] & 0xC0) == 0x80)
            cut--;
        dc_warn("text_edit: set_text truncating %zu bytes to %zu (1 MiB cap)", len, cut);
        len = cut;
    }
    if (!te_grow(te, len)) {
        /* Cap check above should make this unreachable, but never leave the
         * buffer in a half-updated state if it somehow happens. */
        return;
    }
    if (len > 0)
        memcpy(te->buf, text, len);
    te->len = len;
    te->buf[te->len] = '\0';
    te->cursor = 0;
    te->sel_anchor = 0;
    te->preferred_x = -1.0f;
    te->scroll = 0.0f;
    te->scroll_max = 0.0f;
    te->layout_valid = false;
    te->dirty = false; /* freshly loaded from storage -- not "unsaved changes" */
}

const char *dc_text_edit_get_text(const dc_text_edit *te, size_t *out_len)
{
    if (out_len)
        *out_len = te ? te->len : 0;
    return te ? te->buf : "";
}

bool dc_text_edit_is_dirty(const dc_text_edit *te)
{
    return te && te->dirty;
}

void dc_text_edit_mark_clean(dc_text_edit *te)
{
    if (te)
        te->dirty = false;
}

uint64_t dc_text_edit_last_edit_ms(const dc_text_edit *te)
{
    return te ? te->last_edit_ms : 0;
}

void dc_text_edit_scroll(dc_text_edit *te, float dy)
{
    if (!te || dy == 0.0f)
        return;
    float s = te->scroll + dy * TE_SCROLL_STEP;
    if (s < 0.0f)
        s = 0.0f;
    if (s > te->scroll_max)
        s = te->scroll_max;
    te->scroll = s;
}

void dc_text_edit_take_focus(dc_text_edit *te)
{
    if (te)
        te->focused = true;
}

void dc_text_edit_drop_focus(dc_text_edit *te)
{
    if (te)
        te->focused = false;
}

/* ========================================================================
 * Layout cache + cursor math -- everything below here touches the nanovg
 * font atlas (fontstash) and therefore REQUIRES the caller's EGL context
 * current. Not exercised by tests/test_text_edit.c.
 * ======================================================================== */

static void te_rows_reserve(dc_text_edit *te, int need)
{
    if (need <= te->row_cap)
        return;
    int newcap = te->row_cap ? te->row_cap * 2 : TE_ROW_CAP_INITIAL;
    while (newcap < need)
        newcap *= 2;
    te_row *nr = realloc(te->rows, (size_t)newcap * sizeof(te_row));
    if (!nr) {
        dc_warn("text_edit: rows realloc failed (cap %d)", newcap);
        return; /* keep old rows/cap; row-append below is bounds-checked */
    }
    te->rows = nr;
    te->row_cap = newcap;
}

static void te_rows_append(dc_text_edit *te, size_t start, size_t end, float width)
{
    te_rows_reserve(te, te->row_count + 1);
    if (te->row_count >= te->row_cap)
        return; /* reserve failed; drop rather than overrun */
    te->rows[te->row_count++] = (te_row){.start = start, .end = end, .width = width};
}

/* (Re)build the visual-row cache for the whole buffer, wrapped at `wrap_w`.
 * nvgTextBreakLines() treats '\n' as a hard break (verified against
 * third_party/nanovg/nanovg.c) in addition to word-wrapping at wrap_w, so a
 * single pass covers both -- rows are pulled in maxRows-sized chunks (same
 * paging loop nanovg's own nvgTextBox()/nvgTextBoxBounds() use internally)
 * until the whole buffer is consumed. A buffer that ends with a trailing
 * '\n' (or is completely empty) needs one more, empty, synthetic row
 * appended afterward: nvgTextBreakLines only flushes a final row when it
 * still has a pending non-whitespace run at end-of-string, so the empty
 * line implied by a trailing newline (or the single empty line of an empty
 * document) is never emitted by the loop itself. */
static void te_ensure_layout(dc_text_edit *te, struct dc_render *render, float wrap_w)
{
    if (te->layout_valid && te->layout_wrap_w == wrap_w)
        return;

    nvgFontFaceId(render->vg, render->font_ui);
    nvgFontSize(render->vg, TE_FONT_SIZE);
    nvgTextLineHeight(render->vg, TE_LINE_HEIGHT_MULT);
    nvgTextAlign(render->vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    te->row_count = 0;
    const char *cursor_str = te->buf;
    const char *end = te->buf + te->len;
    float safe_wrap_w = wrap_w > 1.0f ? wrap_w : 1.0e6f;

    while (cursor_str < end) {
        NVGtextRow rows[16];
        int n = nvgTextBreakLines(render->vg, cursor_str, end, safe_wrap_w, rows, 16);
        if (n == 0)
            break; /* remaining text is whitespace with no break point -- nothing more to lay out */
        for (int i = 0; i < n; i++)
            te_rows_append(te, (size_t)(rows[i].start - te->buf), (size_t)(rows[i].end - te->buf),
                          rows[i].width);
        cursor_str = rows[n - 1].next;
    }

    if (te->len == 0 || te->buf[te->len - 1] == '\n')
        te_rows_append(te, te->len, te->len, 0.0f);

    te->layout_valid = true;
    te->layout_wrap_w = wrap_w;
}

/* Largest row index i such that rows[i].start <= cursor (rows are stored in
 * increasing `start` order and never overlap) -- naturally prefers the LATER
 * row when `cursor` sits exactly on a row boundary (e.g. right after a '\n'
 * or a wrap point), which is the "cursor renders before the character at
 * this offset" convention this widget uses throughout. */
static int te_row_of_cursor(const dc_text_edit *te, size_t cursor)
{
    if (te->row_count == 0)
        return 0;
    int lo = 0, hi = te->row_count - 1, ans = 0;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (te->rows[mid].start <= cursor) {
            ans = mid;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return ans;
}

/* Load-bearing primitive #1: cursor byte-offset -> x pixel offset within its
 * own row, via nvgTextGlyphPositions' per-glyph advances (unshaped -- v1 is
 * LTR-correct only, see docs/22 §2 RTL caveat). Offset at/beyond the row's
 * last glyph returns the row's total (unshaped) width from the layout cache. */
static float te_cursor_x_in_row(dc_text_edit *te, struct dc_render *render, const te_row *row,
                                size_t cursor)
{
    if (row->end <= row->start || cursor <= row->start)
        return 0.0f;
    nvgFontFaceId(render->vg, render->font_ui);
    nvgFontSize(render->vg, TE_FONT_SIZE);
    NVGglyphPosition pos[TE_GLYPH_SCRATCH];
    int n = nvgTextGlyphPositions(render->vg, 0.0f, 0.0f, te->buf + row->start, te->buf + row->end,
                                  pos, TE_GLYPH_SCRATCH);
    for (int i = 0; i < n; i++) {
        size_t off = (size_t)(pos[i].str - te->buf);
        if (off == cursor)
            return pos[i].x;
        if (off > cursor)
            return i > 0 ? pos[i - 1].x : 0.0f;
    }
    return row->width;
}

/* Load-bearing primitive #2: local (x, y) inside the widget's rect -> the
 * nearest codepoint boundary, via row = (y+scroll)/line_h then a
 * midpoint-rule hit test over that row's per-glyph advances. */
static size_t te_xy_to_cursor(dc_text_edit *te, struct dc_render *render, float x, float y)
{
    te_ensure_layout(te, render, te->layout_wrap_w);
    if (te->row_count == 0)
        return 0;
    float line_h = te_line_h();
    int row = (int)((y + te->scroll) / line_h);
    if (row < 0)
        row = 0;
    if (row >= te->row_count)
        row = te->row_count - 1;
    const te_row *r = &te->rows[row];
    if (r->end <= r->start)
        return r->start;

    nvgFontFaceId(render->vg, render->font_ui);
    nvgFontSize(render->vg, TE_FONT_SIZE);
    NVGglyphPosition pos[TE_GLYPH_SCRATCH];
    int n = nvgTextGlyphPositions(render->vg, 0.0f, 0.0f, te->buf + r->start, te->buf + r->end, pos,
                                  TE_GLYPH_SCRATCH);
    if (n == 0)
        return r->start;
    for (int i = 0; i < n; i++) {
        float glyph_w = (i + 1 < n) ? (pos[i + 1].x - pos[i].x) : (r->width - pos[i].x);
        float mid = pos[i].x + glyph_w * 0.5f;
        if (x < mid)
            return (size_t)(pos[i].str - te->buf);
    }
    return r->end;
}

/* After any edit/cursor-move: keep the cursor's row within [scroll,
 * scroll+viewport_h], and recompute scroll_max against the current row
 * count. Uses the last-drawn viewport height (0 before the first draw(),
 * in which case there's nothing to clamp against yet). */
static void te_scroll_to_cursor(dc_text_edit *te, struct dc_render *render)
{
    te_ensure_layout(te, render, te->layout_wrap_w);
    float line_h = te_line_h();
    float content_h = (float)te->row_count * line_h;
    float h = te->last_draw_h;
    te->scroll_max = (h > 0.0f && content_h > h) ? content_h - h : 0.0f;

    if (h > 0.0f && te->row_count > 0) {
        int row = te_row_of_cursor(te, te->cursor);
        float row_top = (float)row * line_h;
        float row_bot = row_top + line_h;
        if (row_top < te->scroll)
            te->scroll = row_top;
        else if (row_bot > te->scroll + h)
            te->scroll = row_bot - h;
    }
    if (te->scroll < 0.0f)
        te->scroll = 0.0f;
    if (te->scroll > te->scroll_max)
        te->scroll = te->scroll_max;
}

static void te_move_vertical(dc_text_edit *te, struct dc_render *render, uint32_t keysym)
{
    te_ensure_layout(te, render, te->layout_wrap_w);
    if (te->row_count == 0)
        return;
    int row = te_row_of_cursor(te, te->cursor);

    if (te->preferred_x < 0.0f)
        te->preferred_x = te_cursor_x_in_row(te, render, &te->rows[row], te->cursor);
    float x = te->preferred_x;

    int page_rows = te->last_draw_h > 0.0f ? (int)(te->last_draw_h / te_line_h()) : 10;
    if (page_rows < 1)
        page_rows = 1;

    int delta = 0;
    switch (keysym) {
    case XKB_KEY_Up:
        delta = -1;
        break;
    case XKB_KEY_Down:
        delta = 1;
        break;
    case XKB_KEY_Page_Up:
        delta = -page_rows;
        break;
    case XKB_KEY_Page_Down:
        delta = page_rows;
        break;
    default:
        break;
    }

    int new_row = row + delta;
    if (new_row < 0)
        new_row = 0;
    if (new_row >= te->row_count)
        new_row = te->row_count - 1;

    if (new_row == row && delta < 0) {
        te->cursor = te->rows[new_row].start; /* Up/PageUp from the first row: snap to buffer start */
    } else if (new_row == row && delta > 0) {
        te->cursor = te->rows[new_row].end; /* Down/PageDown from the last row: snap to buffer end */
    } else {
        te->cursor = te_xy_to_cursor(te, render, x, (float)new_row * te_line_h() - te->scroll);
    }
    /* te_xy_to_cursor()/set_cursor-style moves reset preferred_x elsewhere;
     * this path must NOT, so Up/Up/Up keeps tracking the original column. */
    te->preferred_x = x;
}

bool dc_text_edit_key(dc_text_edit *te, struct dc_render *render, uint32_t keysym, const char *utf8,
                      bool ctrl, bool shift)
{
    DC_UNUSED(shift); /* selection is a follow-up task (docs/22 NT7); accepted, not yet acted on */
    if (!te || !render)
        return false;

    bool consumed = true;

    switch (keysym) {
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        dc_text_edit_insert_utf8(te, "\n", 1);
        break;
    case XKB_KEY_BackSpace:
        dc_text_edit_delete_backward(te);
        break;
    case XKB_KEY_Delete:
        dc_text_edit_delete_forward(te);
        break;
    case XKB_KEY_Tab:
        dc_text_edit_insert_utf8(te, "    ", 4);
        break;
    case XKB_KEY_Left:
        dc_text_edit_set_cursor(te, dc_text_edit_prev_boundary(te, te->cursor));
        break;
    case XKB_KEY_Right:
        dc_text_edit_set_cursor(te, dc_text_edit_next_boundary(te, te->cursor));
        break;
    case XKB_KEY_Home:
        if (ctrl) {
            dc_text_edit_set_cursor(te, 0);
        } else {
            te_ensure_layout(te, render, te->layout_wrap_w);
            int row = te_row_of_cursor(te, te->cursor);
            dc_text_edit_set_cursor(te, te->rows[row].start);
        }
        break;
    case XKB_KEY_End:
        if (ctrl) {
            dc_text_edit_set_cursor(te, te->len);
        } else {
            te_ensure_layout(te, render, te->layout_wrap_w);
            int row = te_row_of_cursor(te, te->cursor);
            dc_text_edit_set_cursor(te, te->rows[row].end);
        }
        break;
    case XKB_KEY_Up:
    case XKB_KEY_Down:
    case XKB_KEY_Page_Up:
    case XKB_KEY_Page_Down:
        te_move_vertical(te, render, keysym);
        break;
    default:
        if (utf8 && utf8[0] && !((unsigned char)utf8[0] < 0x20) && (unsigned char)utf8[0] != 0x7f) {
            dc_text_edit_insert_utf8(te, utf8, strlen(utf8));
        } else {
            consumed = false;
        }
        break;
    }

    if (consumed)
        te_scroll_to_cursor(te, render);
    return consumed;
}

void dc_text_edit_click(dc_text_edit *te, struct dc_render *render, float x, float y)
{
    if (!te || !render)
        return;
    dc_text_edit_set_cursor(te, te_xy_to_cursor(te, render, x, y));
    te_scroll_to_cursor(te, render);
}

void dc_text_edit_draw(dc_text_edit *te, struct dc_render *render, float x, float y, float w,
                       float h, const char *placeholder)
{
    if (!te || !render || w <= 0.0f || h <= 0.0f)
        return;

    te->last_draw_w = w;
    te->last_draw_h = h;
    te_ensure_layout(te, render, w);
    /* Viewport height may have just changed (panel resize) independent of
     * any cursor move -- keep scroll_max (and scroll's clamp) current every
     * frame, not just after edits/key/click. */
    te_scroll_to_cursor(te, render);

    NVGcontext *vg = render->vg;
    const dc_theme *t = dc_theme_current;
    float line_h = te_line_h();

    nvgSave(vg);
    nvgScissor(vg, x, y, w, h);

    nvgFontFaceId(vg, render->font_ui);
    nvgFontSize(vg, TE_FONT_SIZE);
    nvgTextLineHeight(vg, TE_LINE_HEIGHT_MULT);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);

    if (te->len == 0) {
        if (placeholder && placeholder[0]) {
            nvgFillColor(vg, tc_alpha(t->surface_text, 110));
            nvgText(vg, x, y, placeholder, NULL);
        }
    } else {
        nvgFillColor(vg, tc(t->surface_text));
        for (int i = 0; i < te->row_count; i++) {
            float row_y = y + (float)i * line_h - te->scroll;
            if (row_y + line_h < y || row_y > y + h)
                continue; /* fully outside the viewport -- skip drawing */
            const te_row *r = &te->rows[i];
            if (r->end > r->start)
                dc_shape_draw_text(render, x, row_y, te->buf + r->start, te->buf + r->end);
        }
    }

    if (te->focused && te->row_count > 0) {
        int crow = te_row_of_cursor(te, te->cursor);
        float cursor_y = y + (float)crow * line_h - te->scroll;
        if (cursor_y + line_h >= y && cursor_y <= y + h) {
            float cursor_x = x + te_cursor_x_in_row(te, render, &te->rows[crow], te->cursor);
            nvgBeginPath(vg);
            nvgRect(vg, cursor_x, cursor_y, TE_CURSOR_W, line_h);
            nvgFillColor(vg, tc(t->primary));
            nvgFill(vg);
        }
    }

    if (te->scroll_max > 0.0f) {
        float content_h = (float)te->row_count * line_h;
        float track_x = x + w - TE_SCROLLBAR_W;
        float thumb_h = h * (h / content_h);
        if (thumb_h < 24.0f)
            thumb_h = 24.0f;
        if (thumb_h > h)
            thumb_h = h;
        float thumb_y = y + (h - thumb_h) * (te->scroll / te->scroll_max);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, track_x, thumb_y, TE_SCROLLBAR_W, thumb_h, TE_SCROLLBAR_W * 0.5f);
        nvgFillColor(vg, tc_alpha(t->outline, 140));
        nvgFill(vg);
    }

    nvgRestore(vg);
}
