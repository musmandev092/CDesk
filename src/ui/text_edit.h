/* text_edit.h — reusable multi-line UTF-8 text-editing widget (docs/22-
 * NOTEPAD-PLAN.md §2). The crux piece dankc's Notepad feature is built on:
 * dankc had NO multi-line editor before this.
 *
 * A self-contained widget: contiguous UTF-8 buffer, byte-offset cursor
 * always on a codepoint boundary, a visual-row layout cache (built from
 * nvgTextBreakLines), and scroll state. It draws into a caller-provided rect
 * using the caller's dc_render — there is NO Wayland/surface/layer-shell code
 * in here at all; that belongs to whatever panel embeds this widget (the
 * planned src/ui/notepad.c).
 *
 * *** EGL CONTEXT RULE *** — every function below that isn't listed as a
 * "pure buffer op" touches the nanovg font atlas via fontstash (measuring
 * text to build/consult the row-layout cache). The caller MUST have its EGL
 * context current before calling any of those, exactly like clip_picker.c's
 * cp_purge_thumbnails()/cp_render() precedent (dc_egl_make_current() first).
 * The pure buffer ops are safe to call from anywhere, including from tests
 * that never create a GL context at all.
 *
 * Selection/undo/clipboard are a separate follow-up task (docs/22 NT7):
 * `sel_anchor` exists as a field reserved for that, but no selection logic
 * runs today — Shift is accepted by dc_text_edit_key() but currently has the
 * same effect as without it (plain cursor move, no selection extended).
 */
#ifndef DC_UI_TEXT_EDIT_H
#define DC_UI_TEXT_EDIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dc_render;

/* Hard cap on buffer content bytes (docs/22 §2). An insert that would grow
 * the buffer past this is refused whole (dc_warn logged), never partially
 * applied/truncated mid-insert. */
#define DC_TEXT_EDIT_MAX_BYTES (1u << 20) /* 1 MiB */

/* One visual (wrapped or hard-broken) row, byte-offset range into the live
 * buffer; `end` is one-past-the-last-content-byte (matching NVGtextRow),
 * i.e. it excludes a row-ending '\n'. Cached by te_ensure_layout(). */
typedef struct te_row {
    size_t start, end;
    float width; /* unshaped logical width in px, straight from nvgTextBreakLines */
} te_row;

typedef struct dc_text_edit {
    /* --- buffer (pure ops only touch these) ------------------------------ */
    char *buf;   /* contiguous, always NUL-terminated at buf[len] */
    size_t len;  /* content bytes in use, NOT counting the NUL */
    size_t cap;  /* allocated size; always >= len + 1 */

    size_t cursor;     /* byte offset, always on a codepoint boundary */
    size_t sel_anchor; /* reserved for NT7 (selection); unused today */
    float preferred_x; /* remembered horizontal target for Up/Down/PageUp/Down; <0 = "recompute" */

    bool dirty;
    uint64_t last_edit_ms;
    bool focused;

    /* --- layout cache (needs EGL current to (re)build) -------------------- */
    te_row *rows;
    int row_count, row_cap;
    bool layout_valid;
    float layout_wrap_w; /* wrap width (== last dc_text_edit_draw's `w`) the cache was built for */

    float scroll, scroll_max;
    float last_draw_w, last_draw_h; /* remembered viewport size, for PageUp/Down + click mapping */
} dc_text_edit;

/* --- lifecycle ------------------------------------------------------------ */
void dc_text_edit_init(dc_text_edit *te);
void dc_text_edit_free(dc_text_edit *te);

/* --- pure buffer ops (no GL/EGL/nanovg touched; safe to unit-test) -------- */

/* Replace the whole buffer (for loading from storage). Truncates to
 * DC_TEXT_EDIT_MAX_BYTES (snapped back to a codepoint boundary, + dc_warn) if
 * longer. Resets cursor/scroll to 0 and clears the dirty flag (a freshly
 * loaded document isn't "unsaved changes"). Invalidates the layout cache. */
void dc_text_edit_set_text(dc_text_edit *te, const char *text, size_t len);

/* Borrowed pointer to the live NUL-terminated buffer + its length in bytes
 * (for saving to storage). Valid until the next mutating call. */
const char *dc_text_edit_get_text(const dc_text_edit *te, size_t *out_len);

/* UTF-8 codepoint boundary navigation: skips backward/forward over any
 * 0x80-0xBF continuation bytes so the result always lands on a codepoint
 * start. Clamps `pos` into [0, te->len] first. */
size_t dc_text_edit_prev_boundary(const dc_text_edit *te, size_t pos);
size_t dc_text_edit_next_boundary(const dc_text_edit *te, size_t pos);

/* Move the cursor to `pos`, clamped to [0, len] and snapped backward to the
 * nearest codepoint boundary if it lands mid-sequence. Resets preferred_x. */
void dc_text_edit_set_cursor(dc_text_edit *te, size_t pos);

/* Insert `n` raw bytes (assumed valid UTF-8, e.g. one decoded key's utf8
 * string) at the cursor, advancing it past the inserted text. Grows the
 * buffer by doubling as needed (starting cap picked internally), capped at
 * DC_TEXT_EDIT_MAX_BYTES total content bytes. Returns false (buffer
 * unchanged, dc_warn logged) if this insert would exceed that cap — refused
 * whole, never partially applied. Sets dirty + last_edit_ms, invalidates the
 * layout cache (does NOT rebuild it — that happens lazily under EGL). */
bool dc_text_edit_insert_utf8(dc_text_edit *te, const char *utf8, size_t n);

/* Delete one codepoint before/after the cursor (no-op at a buffer edge). */
void dc_text_edit_delete_backward(dc_text_edit *te);
void dc_text_edit_delete_forward(dc_text_edit *te);

bool dc_text_edit_is_dirty(const dc_text_edit *te);
void dc_text_edit_mark_clean(dc_text_edit *te);
uint64_t dc_text_edit_last_edit_ms(const dc_text_edit *te);

/* Wheel scroll, clamped to [0, scroll_max] (scroll_max is whatever the most
 * recent draw()/key() computed). Pure — no GL touched. */
void dc_text_edit_scroll(dc_text_edit *te, float dy);

void dc_text_edit_take_focus(dc_text_edit *te);
void dc_text_edit_drop_focus(dc_text_edit *te);

/* --- ops that need EGL current (build/consult the row-layout cache) ------- */

/* Handle one decoded key event: `keysym` + its already-UTF-8-decoded text (if
 * any), exactly like dc_clip_picker_handle_key()'s (keysym, utf8) pair.
 * ctrl/shift are passed as separate bools since the wl.c modifier-tracking
 * helpers are a different task (docs/22 NT0); shift is accepted for a future
 * selection follow-up but has no effect yet. Returns true if the key was
 * consumed (caller should re-render). Requires EGL current + a prior
 * dc_text_edit_draw() call this session (Up/Down/PageUp/Down/Home/End need
 * the row layout, keyed off the widget's last-drawn width). */
bool dc_text_edit_key(dc_text_edit *te, struct dc_render *render, uint32_t keysym, const char *utf8,
                      bool ctrl, bool shift);

/* Map a click at (x, y) — logical coordinates relative to the widget's own
 * rect, i.e. already offset by whatever (x, y) dc_text_edit_draw() was last
 * called with — to the nearest codepoint boundary and move the cursor there.
 * Requires EGL current + a prior dc_text_edit_draw() call. */
void dc_text_edit_click(dc_text_edit *te, struct dc_render *render, float x, float y);

/* Draw into the caller's rect (x, y, w, h in the caller's own logical
 * coordinate space, i.e. between that surface's nvgBeginFrame/nvgEndFrame).
 * Requires EGL current + the caller's dc_render_ensure() already run this
 * frame (same convention as every other popout's draw function). Draws only
 * the rows intersecting the current scroll window, a 2px cursor when
 * focused, and a scrollbar thumb when the content overflows `h`.
 * `placeholder`, if non-NULL, is shown dimmed when the buffer is empty. */
void dc_text_edit_draw(dc_text_edit *te, struct dc_render *render, float x, float y, float w,
                       float h, const char *placeholder);

#endif /* DC_UI_TEXT_EDIT_H */
