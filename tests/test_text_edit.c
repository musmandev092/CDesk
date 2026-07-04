/* Standalone unit test for src/ui/text_edit.c's PURE buffer ops (docs/22-
 * NOTEPAD-PLAN.md §2 / task NT1): UTF-8 codepoint boundary navigation,
 * insert/delete at the cursor, buffer growth-by-doubling + the 1 MiB hard
 * cap, and cursor clamping. Deliberately exercises none of the EGL-dependent
 * layout/draw/key/click code (dc_text_edit_key/click/draw, te_ensure_layout
 * and everything built on it) -- see tests/test_text_edit_stubs.c for why
 * this binary still links cleanly without a real nanovg/GL/HarfBuzz/
 * Fontconfig stack, same standalone spirit as tests/test_calc.c. */
#include "ui/text_edit.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, ...)                                                                         \
    do {                                                                                         \
        g_checks++;                                                                              \
        if (!(cond)) {                                                                           \
            printf("FAIL: ");                                                                    \
            printf(__VA_ARGS__);                                                                 \
            printf("\n");                                                                        \
            g_failures++;                                                                        \
        } else {                                                                                 \
            printf("ok:   ");                                                                    \
            printf(__VA_ARGS__);                                                                 \
            printf("\n");                                                                        \
        }                                                                                        \
    } while (0)

/* --- UTF-8 boundary navigation -------------------------------------------- */

static void test_utf8_boundaries(void)
{
    dc_text_edit te;
    dc_text_edit_init(&te);

    /* "a" (1B) + "\xc3\xa9" e-acute (2B) + "\xe4\xb8\xad" CJK (3B)
     *   + "\xf0\x9f\x98\x80" emoji (4B) + "b" (1B) */
    const char *s = "a\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80"
                    "b";
    size_t len = strlen(s);
    dc_text_edit_set_text(&te, s, len);
    CHECK(te.len == len, "set_text stored %zu bytes (want %zu)", te.len, len);

    /* Byte offsets: 0='a', 1..2=e-acute, 3..5=CJK, 6..9=emoji, 10='b', 11=end. */
    CHECK(dc_text_edit_next_boundary(&te, 0) == 1, "next_boundary(0) == 1 (past 'a')");
    CHECK(dc_text_edit_next_boundary(&te, 1) == 3, "next_boundary(1) == 3 (past 2-byte e-acute)");
    CHECK(dc_text_edit_next_boundary(&te, 3) == 6, "next_boundary(3) == 6 (past 3-byte CJK)");
    CHECK(dc_text_edit_next_boundary(&te, 6) == 10, "next_boundary(6) == 10 (past 4-byte emoji)");
    CHECK(dc_text_edit_next_boundary(&te, 10) == 11, "next_boundary(10) == 11 (past 'b')");
    CHECK(dc_text_edit_next_boundary(&te, 11) == 11, "next_boundary at end stays at end");

    CHECK(dc_text_edit_prev_boundary(&te, 11) == 10, "prev_boundary(11) == 10 (back over 'b')");
    CHECK(dc_text_edit_prev_boundary(&te, 10) == 6, "prev_boundary(10) == 6 (back over emoji)");
    CHECK(dc_text_edit_prev_boundary(&te, 6) == 3, "prev_boundary(6) == 3 (back over CJK)");
    CHECK(dc_text_edit_prev_boundary(&te, 3) == 1, "prev_boundary(3) == 1 (back over e-acute)");
    CHECK(dc_text_edit_prev_boundary(&te, 1) == 0, "prev_boundary(1) == 0 (back over 'a')");
    CHECK(dc_text_edit_prev_boundary(&te, 0) == 0, "prev_boundary at start stays at start");

    /* Landing mid-sequence (e.g. a stale/miscomputed offset) must snap to
     * the boundary, never dereference/return a continuation byte offset. */
    CHECK(dc_text_edit_next_boundary(&te, 2) == 3, "next_boundary from mid e-acute snaps to 3");
    CHECK(dc_text_edit_prev_boundary(&te, 2) == 1, "prev_boundary from mid e-acute snaps to 1");
    CHECK(dc_text_edit_next_boundary(&te, 4) == 6, "next_boundary from mid CJK snaps to 6");
    CHECK(dc_text_edit_prev_boundary(&te, 8) == 6, "prev_boundary from mid emoji snaps to 6");

    dc_text_edit_free(&te);
}

/* --- insert/delete at the cursor ------------------------------------------ */

static void test_insert_delete(void)
{
    dc_text_edit te;
    dc_text_edit_init(&te);

    CHECK(te.len == 0 && te.cursor == 0, "freshly init'd editor is empty");

    CHECK(dc_text_edit_insert_utf8(&te, "hello", 5), "insert 'hello' succeeds");
    CHECK(te.len == 5 && te.cursor == 5 && strcmp(te.buf, "hello") == 0,
          "buffer == 'hello', cursor at end (got '%s', cursor %zu)", te.buf, te.cursor);

    /* Insert mid-buffer: move cursor back 5 (== to 0), insert at the front. */
    dc_text_edit_set_cursor(&te, 0);
    CHECK(dc_text_edit_insert_utf8(&te, "say ", 4), "insert 'say ' at the front succeeds");
    CHECK(strcmp(te.buf, "say hello") == 0 && te.cursor == 4,
          "buffer == 'say hello' (got '%s'), cursor after inserted text (got %zu)", te.buf,
          te.cursor);

    /* Multi-byte insert: CJK "\xe4\xb8\xad" (3 bytes) in the middle. */
    dc_text_edit_set_cursor(&te, 4); /* right after "say " */
    CHECK(dc_text_edit_insert_utf8(&te, "\xe4\xb8\xad", 3), "insert 3-byte CJK codepoint succeeds");
    CHECK(strcmp(te.buf, "say \xe4\xb8\xad"
                        "hello") == 0,
          "buffer == 'say <CJK>hello' (got '%s')", te.buf);
    CHECK(te.cursor == 7, "cursor advanced by the whole 3-byte insert (got %zu)", te.cursor);

    /* delete_backward at this cursor must remove the WHOLE codepoint (3
     * bytes), not just its last byte. */
    dc_text_edit_delete_backward(&te);
    CHECK(strcmp(te.buf, "say hello") == 0 && te.cursor == 4,
          "delete_backward removed the whole CJK codepoint (got '%s', cursor %zu)", te.buf,
          te.cursor);

    /* delete_forward similarly removes a whole codepoint ahead of the cursor. */
    dc_text_edit_insert_utf8(&te, "\xf0\x9f\x98\x80", 4); /* 4-byte emoji, cursor now after it */
    dc_text_edit_set_cursor(&te, 4);                       /* cursor before the emoji */
    CHECK(strncmp(te.buf + 4, "\xf0\x9f\x98\x80", 4) == 0, "emoji landed right after 'say '");
    dc_text_edit_delete_forward(&te);
    CHECK(strcmp(te.buf, "say hello") == 0, "delete_forward removed the whole 4-byte emoji (got '%s')",
          te.buf);

    /* Deleting at the very start/end is a no-op, not a crash/underflow. */
    dc_text_edit_set_cursor(&te, 0);
    dc_text_edit_delete_backward(&te);
    CHECK(strcmp(te.buf, "say hello") == 0, "delete_backward at offset 0 is a no-op");
    dc_text_edit_set_cursor(&te, te.len);
    dc_text_edit_delete_forward(&te);
    CHECK(strcmp(te.buf, "say hello") == 0, "delete_forward at end-of-buffer is a no-op");

    /* dirty/last_edit_ms bookkeeping. */
    CHECK(dc_text_edit_is_dirty(&te), "editor is dirty after edits");
    dc_text_edit_mark_clean(&te);
    CHECK(!dc_text_edit_is_dirty(&te), "mark_clean clears the dirty flag");
    dc_text_edit_insert_utf8(&te, "!", 1);
    CHECK(dc_text_edit_is_dirty(&te), "editor is dirty again after another edit");

    dc_text_edit_free(&te);
}

/* --- buffer growth-by-doubling + the 1 MiB hard cap ----------------------- */

static void test_growth_and_cap(void)
{
    dc_text_edit te;
    dc_text_edit_init(&te);

    size_t initial_cap = te.cap;
    CHECK(initial_cap > 0, "editor starts with a non-zero initial capacity (%zu)", initial_cap);

    /* Insert past the initial capacity and confirm it grows (by doubling,
     * i.e. some power-of-two multiple of the initial cap) rather than
     * failing or truncating. */
    char chunk[64];
    memset(chunk, 'x', sizeof(chunk));
    size_t total = 0;
    while (total < initial_cap + 32) {
        CHECK(dc_text_edit_insert_utf8(&te, chunk, sizeof(chunk)), "insert chunk #%zu succeeds",
              total / sizeof(chunk));
        total += sizeof(chunk);
    }
    CHECK(te.len == total, "buffer length tracks every inserted byte (got %zu, want %zu)", te.len,
          total);
    CHECK(te.cap > initial_cap, "capacity grew past the initial %zu (now %zu)", initial_cap, te.cap);
    CHECK((te.cap & (te.cap - 1)) == 0, "capacity stayed a power of two after doubling (%zu)",
          te.cap);
    CHECK(te.cap >= te.len + 1, "capacity always leaves room for the NUL terminator");

    dc_text_edit_free(&te);

    /* Hard cap: fill exactly to DC_TEXT_EDIT_MAX_BYTES, then confirm a
     * further insert is refused WHOLE (no partial write, length unchanged)
     * and a direct set_text() past the cap truncates instead of overflowing. */
    dc_text_edit_init(&te);
    char *big = malloc(DC_TEXT_EDIT_MAX_BYTES);
    memset(big, 'y', DC_TEXT_EDIT_MAX_BYTES);
    dc_text_edit_set_text(&te, big, DC_TEXT_EDIT_MAX_BYTES);
    CHECK(te.len == DC_TEXT_EDIT_MAX_BYTES, "set_text fills exactly to the 1 MiB cap (got %zu)",
          te.len);

    bool ok = dc_text_edit_insert_utf8(&te, "z", 1);
    CHECK(!ok, "insert beyond the 1 MiB cap is refused");
    CHECK(te.len == DC_TEXT_EDIT_MAX_BYTES, "buffer length unchanged after the refused insert (got %zu)",
          te.len);

    dc_text_edit_free(&te);

    /* set_text() with MORE than the cap truncates down to it (snapped to a
     * codepoint boundary) rather than refusing outright or overflowing. */
    dc_text_edit_init(&te);
    memset(big, 'y', DC_TEXT_EDIT_MAX_BYTES);
    dc_text_edit_set_text(&te, big, DC_TEXT_EDIT_MAX_BYTES + 100);
    CHECK(te.len <= DC_TEXT_EDIT_MAX_BYTES, "set_text() past the cap truncates (got %zu, cap %u)",
          te.len, (unsigned)DC_TEXT_EDIT_MAX_BYTES);

    free(big);
    dc_text_edit_free(&te);
}

/* --- cursor clamping -------------------------------------------------------- */

static void test_cursor_clamping(void)
{
    dc_text_edit te;
    dc_text_edit_init(&te);

    const char *s = "ab\xc3\xa9girl"; /* 'a','b', e-acute (2B), "girl" */
    dc_text_edit_set_text(&te, s, strlen(s));

    dc_text_edit_set_cursor(&te, 9999);
    CHECK(te.cursor == te.len, "set_cursor clamps an out-of-range offset to the buffer length");

    /* Byte 2 is the FIRST byte of the 2-byte e-acute (a valid boundary);
     * byte 3 is its continuation byte and must snap back to 2. */
    dc_text_edit_set_cursor(&te, 2);
    CHECK(te.cursor == 2, "set_cursor accepts a real codepoint boundary unchanged");
    dc_text_edit_set_cursor(&te, 3);
    CHECK(te.cursor == 2, "set_cursor snaps a mid-codepoint offset back to its boundary (got %zu)",
          te.cursor);

    dc_text_edit_set_cursor(&te, 0);
    CHECK(te.cursor == 0, "set_cursor accepts offset 0");

    dc_text_edit_free(&te);
}

int main(void)
{
    test_utf8_boundaries();
    test_insert_delete();
    test_growth_and_cap();
    test_cursor_clamping();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
