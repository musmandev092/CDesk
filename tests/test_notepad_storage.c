/* Standalone unit test for src/services/notepad_storage.c (docs/22-NOTEPAD-
 * PLAN.md sec.3). Deliberately built without touching any other dankc module
 * (see the manual gcc invocation in the task report / a would-be
 * `test-notepad-storage` Makefile target mirroring test_calc's), just
 * notepad_storage.c + cJSON.c + this driver -- no Wayland/EGL/protocol
 * machinery needed.
 *
 * IMPORTANT: run with XDG_STATE_HOME pointed at a scratch directory (e.g.
 * `XDG_STATE_HOME=$(mktemp -d) ./bin/test_notepad_storage`) so this never
 * touches the real ~/.local/state/dankc. */
#include "services/notepad_storage.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, msg)                                                                         \
    do {                                                                                          \
        g_checks++;                                                                               \
        if (!(cond)) {                                                                            \
            printf("FAIL: %s (%s:%d)\n", msg, __FILE__, __LINE__);                                \
            g_failures++;                                                                          \
        } else {                                                                                   \
            printf("ok:   %s\n", msg);                                                             \
        }                                                                                           \
    } while (0)

int main(void)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    if (!xdg || !*xdg) {
        fprintf(stderr, "refusing to run: set XDG_STATE_HOME to a scratch dir first\n");
        return 2;
    }

    /* --- fresh load: default tab created --- */
    dc_notepad_storage *st = dc_notepad_storage_load();
    CHECK(st != NULL, "load() never returns NULL");
    CHECK(dc_notepad_storage_count(st) == 1, "fresh load creates exactly one default tab");
    CHECK(dc_notepad_storage_current(st) == 0, "fresh load's current index is 0");
    CHECK(strcmp(dc_notepad_storage_title(st, 0), "Note 1") == 0, "default tab titled 'Note 1'");

    char *content = dc_notepad_storage_read(st, 0);
    CHECK(content != NULL && strcmp(content, "") == 0, "unwritten tab reads back as \"\"");
    free(content);

    /* --- write + read round-trip, atomicity (no stray .tmp left behind) --- */
    CHECK(dc_notepad_storage_write(st, 0, "hello notepad\nline two"), "write() succeeds");
    content = dc_notepad_storage_read(st, 0);
    CHECK(content != NULL && strcmp(content, "hello notepad\nline two") == 0,
          "read() returns exactly what was written");
    free(content);

    /* --- create a second tab, rename it, switch current --- */
    int idx2 = dc_notepad_storage_create_tab(st);
    CHECK(idx2 == 1, "create_tab() returns the new (2nd) index");
    CHECK(dc_notepad_storage_count(st) == 2, "count() is now 2");
    dc_notepad_storage_rename(st, 1, "My Second Note");
    CHECK(strcmp(dc_notepad_storage_title(st, 1), "My Second Note") == 0, "rename() applies");
    dc_notepad_storage_set_current(st, 1);
    CHECK(dc_notepad_storage_current(st) == 1, "set_current() applies");

    CHECK(dc_notepad_storage_write(st, 1, "second tab content"), "write() to 2nd tab succeeds");

    /* --- title bound: an absurdly long title gets truncated, not overflowed --- */
    char huge_title[4096];
    memset(huge_title, 'x', sizeof(huge_title) - 1);
    huge_title[sizeof(huge_title) - 1] = '\0';
    dc_notepad_storage_rename(st, 1, huge_title);
    CHECK(strlen(dc_notepad_storage_title(st, 1)) < 200, "rename() bounds title length");

    dc_notepad_storage_destroy(st);

    /* --- reload: metadata + content persisted across a fresh load() --- */
    st = dc_notepad_storage_load();
    CHECK(dc_notepad_storage_count(st) == 2, "reload sees both tabs (metadata persisted)");
    content = dc_notepad_storage_read(st, 0);
    CHECK(content != NULL && strcmp(content, "hello notepad\nline two") == 0,
          "reload: 1st tab content persisted");
    free(content);
    content = dc_notepad_storage_read(st, 1);
    CHECK(content != NULL && strcmp(content, "second tab content") == 0,
          "reload: 2nd tab content persisted");
    free(content);
    CHECK(dc_notepad_storage_current(st) == 1, "reload: currentTabIndex persisted");

    /* --- delete a tab: file removed, never zero tabs --- */
    dc_notepad_storage_delete_tab(st, 1);
    CHECK(dc_notepad_storage_count(st) == 1, "delete_tab() removes the tab");
    CHECK(dc_notepad_storage_current(st) == 0, "delete_tab() clamps current index");

    /* Delete the very last tab: a fresh default must replace it. */
    dc_notepad_storage_delete_tab(st, 0);
    CHECK(dc_notepad_storage_count(st) == 1, "deleting the last tab recreates a default (never 0)");
    content = dc_notepad_storage_read(st, 0);
    CHECK(content != NULL && strcmp(content, "") == 0, "the recreated default tab is empty");
    free(content);

    dc_notepad_storage_destroy(st);

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures ? 1 : 0;
}
