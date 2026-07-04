/* Standalone unit test for src/services/systheme.c's PURE and file-IO
 * helpers (system-theming engine, see systheme.h/systheme_internal.h):
 *   - dc_systheme_hex_rgb / dc_systheme_hex_argb hex formatting
 *   - dc_systheme_prefers_black_text contrast pick
 *   - dc_systheme_ensure_line / dc_systheme_ensure_line_top marker-line
 *     injector idempotency + once-only backup contract
 *   - dc_systheme_write_owned atomic write + DANKC_THEME_DRYRUN no-op
 *
 * Deliberately does NOT exercise dc_systheme_apply() or
 * dc_systheme_app_detected() (those touch real per-app config dirs/
 * $PATH/fork+exec) -- see tests/test_systheme_stubs.c for why this
 * translation unit still needs a handful of link-only no-op stand-ins for
 * the per-app emitters/dc_config_light_mode()/dc_theme_current systheme.c
 * references but this test never calls, same standalone spirit as
 * tests/test_calc.c and tests/test_text_edit.c.
 *
 * All file-IO tests run against a scratch temp directory (mkdtemp'd under
 * $TMPDIR/or /tmp), never touching a real $HOME/.config -- and every test
 * that isn't specifically exercising DANKC_THEME_DRYRUN itself runs with it
 * UNSET, matching systheme.c's real non-dryrun write path.
 */
#include "services/systheme.h"
#include "services/systheme_internal.h"

#include "theme/theme.h"

#include <errno.h>
#include <glob.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

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

static dc_color mkcolor(int r, int g, int b, int a)
{
    dc_color c;
    c.r = (unsigned char)r;
    c.g = (unsigned char)g;
    c.b = (unsigned char)b;
    c.a = (unsigned char)a;
    return c;
}

/* --- hex formatting --------------------------------------------------------- */

static void test_hex_formatting(void)
{
    char buf[10];

    dc_systheme_hex_rgb(mkcolor(0xde, 0xad, 0xbe, 0xff), buf);
    CHECK(strcmp(buf, "#deadbe") == 0, "hex_rgb(0xde,0xad,0xbe) == \"#deadbe\" (got \"%s\")", buf);

    /* Zero-padding: every component under 0x10 must keep its leading zero. */
    dc_systheme_hex_rgb(mkcolor(0x0a, 0x0b, 0x0c, 0xff), buf);
    CHECK(strcmp(buf, "#0a0b0c") == 0, "hex_rgb zero-pads single-hex-digit components (got \"%s\")",
          buf);

    dc_systheme_hex_rgb(mkcolor(0, 0, 0, 0xff), buf);
    CHECK(strcmp(buf, "#000000") == 0, "hex_rgb(0,0,0) == \"#000000\" (got \"%s\")", buf);

    dc_systheme_hex_rgb(mkcolor(0xff, 0xff, 0xff, 0xff), buf);
    CHECK(strcmp(buf, "#ffffff") == 0, "hex_rgb(255,255,255) == \"#ffffff\" (got \"%s\")", buf);

    /* hex_argb: alpha FIRST (Qt/kitty-style ARGB), same zero-padding rule,
     * and alpha itself is not silently dropped. */
    char abuf[10];
    dc_systheme_hex_argb(mkcolor(0x0a, 0x0b, 0x0c, 0xff), abuf);
    CHECK(strcmp(abuf, "#ff0a0b0c") == 0, "hex_argb puts alpha first (got \"%s\")", abuf);

    dc_systheme_hex_argb(mkcolor(0xde, 0xad, 0xbe, 0x00), abuf);
    CHECK(strcmp(abuf, "#00deadbe") == 0, "hex_argb zero-pads a zero alpha byte (got \"%s\")", abuf);

    dc_systheme_hex_argb(mkcolor(0x12, 0x34, 0x56, 0x78), abuf);
    CHECK(strcmp(abuf, "#78123456") == 0, "hex_argb full round-trip (got \"%s\")", abuf);
}

/* --- contrast pick ----------------------------------------------------------- */

static void test_contrast_pick(void)
{
    CHECK(dc_systheme_prefers_black_text(mkcolor(255, 255, 255, 255)) == true,
          "pure white prefers black text");
    CHECK(dc_systheme_prefers_black_text(mkcolor(0, 0, 0, 255)) == false,
          "pure black prefers white text");

    /* luma = 0.299r + 0.587g + 0.114b; threshold is luma > 140. A flat gray
     * makes luma == the gray value exactly, so this pins the threshold to
     * the byte. */
    CHECK(dc_systheme_prefers_black_text(mkcolor(141, 141, 141, 255)) == true,
          "gray 141 (luma 141 > 140) prefers black text");
    CHECK(dc_systheme_prefers_black_text(mkcolor(140, 140, 140, 255)) == false,
          "gray 140 (luma 140, not > 140) prefers white text");
    CHECK(dc_systheme_prefers_black_text(mkcolor(139, 139, 139, 255)) == false,
          "gray 139 (luma 139 < 140) prefers white text");

    /* A light, saturated swatch and a dark, saturated swatch, to exercise
     * the weighted (not flat-average) luma formula on non-gray colors. */
    CHECK(dc_systheme_prefers_black_text(mkcolor(255, 255, 150, 255)) == true,
          "pale yellow (light) prefers black text");
    CHECK(dc_systheme_prefers_black_text(mkcolor(20, 10, 40, 255)) == false,
          "dark violet prefers white text");
}

/* --- scratch-dir plumbing ---------------------------------------------------- */

static char g_scratch_dir[512];

static void scratch_setup(void)
{
    snprintf(g_scratch_dir, sizeof(g_scratch_dir), "/tmp/dankc_systheme_test_XXXXXX");
    if (!mkdtemp(g_scratch_dir)) {
        fprintf(stderr, "FATAL: mkdtemp failed: %s\n", strerror(errno));
        exit(1);
    }
}

static void scratch_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_scratch_dir, name);
}

/* Remove every file matching "<path>*" (the file itself plus any
 * .bak-<epoch>/.tmp siblings) so repeated test runs never see stale state
 * from a previous invocation. */
static void scratch_clean(const char *path)
{
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s*", path);
    glob_t g;
    if (glob(pattern, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++)
            unlink(g.gl_pathv[i]);
        globfree(&g);
    }
}

static void scratch_teardown(void)
{
    rmdir(g_scratch_dir);
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)size + 1);
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static int count_occurrences(const char *haystack, const char *needle)
{
    int n = 0;
    const char *p = haystack;
    size_t nlen = strlen(needle);
    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += nlen;
    }
    return n;
}

static int count_backups(const char *path)
{
    char pattern[600];
    snprintf(pattern, sizeof(pattern), "%s.bak-*", path);
    glob_t g;
    int n = 0;
    if (glob(pattern, 0, NULL, &g) == 0)
        n = (int)g.gl_pathc;
    globfree(&g);
    return n;
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    fputs(content, f);
    fclose(f);
}

/* --- dc_systheme_ensure_line() idempotency ---------------------------------- */

static void test_ensure_line(void)
{
    unsetenv("DANKC_THEME_DRYRUN");

    char path[560];
    scratch_path(path, sizeof(path), "ensure_line.css");
    scratch_clean(path);

    const char *needle = "@import 'dank-colors.css';";
    const char *marker = "/* Added by DankC Settings > Theme & Colors */";

    /* (b')/setup: pre-existing, user-authored file WITHOUT the needle. */
    const char *original = "/* my hand-written gtk.css */\n.foo { color: red; }\n";
    write_file(path, original);

    /* (a) absent -> added, plus (c) exactly one backup before the first
     * edit of a pre-existing file. */
    bool ok = dc_systheme_ensure_line(path, needle, marker, needle);
    CHECK(ok, "ensure_line() on absent needle returns true");

    char *content = read_file(path);
    CHECK(content && strstr(content, original) != NULL,
          "ensure_line() preserves the original file content");
    CHECK(content && strstr(content, marker) != NULL, "ensure_line() adds the marker comment");
    CHECK(content && strstr(content, needle) != NULL, "ensure_line() adds the needle/import line");
    CHECK(content && count_occurrences(content, needle) == 1,
          "needle appears exactly once after the first edit");

    CHECK(count_backups(path) == 1,
          "exactly one .bak-<epoch> exists after the first edit of a pre-existing file");

    /* Snapshot the backup's content: it must be the ORIGINAL, unmodified
     * file. */
    char backup_pattern[600];
    snprintf(backup_pattern, sizeof(backup_pattern), "%s.bak-*", path);
    glob_t bg;
    glob(backup_pattern, 0, NULL, &bg);
    char *backup_content = bg.gl_pathc ? read_file(bg.gl_pathv[0]) : NULL;
    CHECK(backup_content && strcmp(backup_content, original) == 0,
          "the backup file's content is exactly the pre-edit original");
    free(backup_content);
    globfree(&bg);

    char *content_after_first = content ? strdup(content) : NULL;
    free(content);

    /* (b) needle already present -> pure no-op: no duplicate line, no
     * second backup, file content byte-for-byte unchanged. */
    ok = dc_systheme_ensure_line(path, needle, marker, needle);
    CHECK(ok, "ensure_line() is still reported as success when the needle is already present");

    char *content2 = read_file(path);
    CHECK(content2 && content_after_first && strcmp(content2, content_after_first) == 0,
          "re-running ensure_line() leaves file content byte-for-byte unchanged");
    CHECK(content2 && count_occurrences(content2, needle) == 1,
          "needle still appears exactly once (no duplicate line) after a second call");
    CHECK(count_backups(path) == 1, "no SECOND backup was created on the idempotent no-op call");

    free(content2);
    free(content_after_first);

    scratch_clean(path);

    /* File doesn't exist yet: ensure_line() creates it fresh (marker+line
     * only), with nothing to back up. */
    char path2[560];
    scratch_path(path2, sizeof(path2), "ensure_line_new.css");
    scratch_clean(path2);
    CHECK(!file_exists(path2), "sanity: the fresh-file target doesn't exist yet");

    ok = dc_systheme_ensure_line(path2, needle, marker, needle);
    CHECK(ok, "ensure_line() on a non-existent file returns true");
    CHECK(file_exists(path2), "ensure_line() created the file");
    char *fresh = read_file(path2);
    CHECK(fresh && strstr(fresh, marker) != NULL && strstr(fresh, needle) != NULL,
          "the newly-created file has the marker + needle");
    CHECK(count_backups(path2) == 0, "no backup is created when there was no pre-existing file");
    free(fresh);

    scratch_clean(path);
    scratch_clean(path2);
}

/* --- dc_systheme_ensure_line_top(): same contract, line at the FILE HEAD --- */

static void test_ensure_line_top(void)
{
    unsetenv("DANKC_THEME_DRYRUN");

    char path[560];
    scratch_path(path, sizeof(path), "ensure_top.conf");
    scratch_clean(path);

    const char *line = "include=dank-colors.conf";
    const char *marker = "# Added by DankC Settings > Theme & Colors";
    const char *original = "font=monospace 10\nopacity=0.95\n";
    write_file(path, original);

    bool ok = dc_systheme_ensure_line_top(path, line, marker);
    CHECK(ok, "ensure_line_top() on absent line returns true");

    char *content = read_file(path);
    CHECK(content != NULL, "file readable after ensure_line_top()");
    /* Must land at the very head: marker is byte 0. */
    CHECK(content && strncmp(content, marker, strlen(marker)) == 0,
          "ensure_line_top() places the marker at byte 0 of the file (got head: \"%.40s\")",
          content ? content : "(null)");
    /* And the line must immediately follow the marker line, still ahead of
     * the original content. */
    char head_expect[256];
    snprintf(head_expect, sizeof(head_expect), "%s\n%s\n", marker, line);
    CHECK(content && strncmp(content, head_expect, strlen(head_expect)) == 0,
          "marker+line occupy exactly the first two lines, in order");
    CHECK(content && strstr(content, original) == content + strlen(head_expect),
          "original content follows immediately after the injected head, untouched");

    CHECK(count_backups(path) == 1, "exactly one backup before the first edit");
    char backup_pattern[600];
    snprintf(backup_pattern, sizeof(backup_pattern), "%s.bak-*", path);
    glob_t bg;
    glob(backup_pattern, 0, NULL, &bg);
    char *backup_content = bg.gl_pathc ? read_file(bg.gl_pathv[0]) : NULL;
    CHECK(backup_content && strcmp(backup_content, original) == 0,
          "the backup holds the pre-edit original content");
    free(backup_content);
    globfree(&bg);

    char *content_after_first = content ? strdup(content) : NULL;
    free(content);

    /* Idempotent: line already present -> no-op, no duplicate, no 2nd backup. */
    ok = dc_systheme_ensure_line_top(path, line, marker);
    CHECK(ok, "ensure_line_top() reports success when the line is already present");
    char *content2 = read_file(path);
    CHECK(content2 && content_after_first && strcmp(content2, content_after_first) == 0,
          "re-running ensure_line_top() leaves the file byte-for-byte unchanged");
    CHECK(content2 && count_occurrences(content2, line) == 1,
          "the line still appears exactly once (no duplicate) after a second call");
    CHECK(count_backups(path) == 1, "no second backup created on the idempotent no-op call");
    free(content2);
    free(content_after_first);

    scratch_clean(path);

    /* Non-existent file: created fresh, nothing to back up. */
    char path2[560];
    scratch_path(path2, sizeof(path2), "ensure_top_new.conf");
    scratch_clean(path2);
    ok = dc_systheme_ensure_line_top(path2, line, marker);
    CHECK(ok, "ensure_line_top() on a non-existent file returns true");
    char *fresh = read_file(path2);
    char expect[256];
    snprintf(expect, sizeof(expect), "%s\n%s\n", marker, line);
    CHECK(fresh && strcmp(fresh, expect) == 0,
          "a freshly-created file is EXACTLY marker+line (got \"%s\")", fresh ? fresh : "(null)");
    CHECK(count_backups(path2) == 0, "no backup for a file that didn't previously exist");
    free(fresh);

    scratch_clean(path);
    scratch_clean(path2);
}

/* --- dc_systheme_write_owned(): atomic write + DRYRUN no-op ----------------- */

static void test_write_owned(void)
{
    unsetenv("DANKC_THEME_DRYRUN");

    char path[560];
    scratch_path(path, sizeof(path), "owned.css");
    scratch_clean(path);

    const char *data = "@define-color accent_bg_color #112233;\n";
    bool ok = dc_systheme_write_owned(path, data, strlen(data));
    CHECK(ok, "write_owned() returns true on a real (non-dryrun) write");
    CHECK(file_exists(path), "write_owned() actually created the file");

    char *got = read_file(path);
    CHECK(got && strcmp(got, data) == 0, "write_owned() wrote exactly the given bytes (got \"%s\")",
          got ? got : "(null)");
    free(got);

    /* Overwrite with different, shorter content: the old bytes must not
     * linger past the end (this is a full rewrite, not an append). */
    const char *data2 = "short\n";
    ok = dc_systheme_write_owned(path, data2, strlen(data2));
    CHECK(ok, "write_owned() returns true when overwriting an existing file");
    got = read_file(path);
    CHECK(got && strcmp(got, data2) == 0,
          "write_owned() fully replaces prior content, no leftover trailing bytes (got \"%s\")",
          got ? got : "(null)");
    free(got);

    /* No stray "<path>.tmp" left behind after a successful atomic write. */
    char tmp_path[600];
    snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);
    CHECK(!file_exists(tmp_path), "no leftover .tmp file after a successful atomic write");

    scratch_clean(path);

    /* DANKC_THEME_DRYRUN=1: must write NOTHING -- the target file must not
     * be created at all. */
    setenv("DANKC_THEME_DRYRUN", "1", 1);
    CHECK(!file_exists(path), "sanity: dryrun target doesn't exist yet");
    ok = dc_systheme_write_owned(path, data, strlen(data));
    CHECK(ok, "write_owned() under DANKC_THEME_DRYRUN=1 still reports success");
    CHECK(!file_exists(path), "write_owned() under DANKC_THEME_DRYRUN=1 creates no file");
    char tmp_path2[600];
    snprintf(tmp_path2, sizeof(tmp_path2), "%s.tmp", path);
    CHECK(!file_exists(tmp_path2), "write_owned() under DANKC_THEME_DRYRUN=1 leaves no .tmp either");
    unsetenv("DANKC_THEME_DRYRUN");

    /* DRYRUN also applies to a file that already exists: it must be left
     * completely untouched. */
    write_file(path, "pre-existing, must survive dryrun untouched\n");
    char *before = read_file(path);
    setenv("DANKC_THEME_DRYRUN", "1", 1);
    ok = dc_systheme_write_owned(path, "would clobber this\n", strlen("would clobber this\n"));
    CHECK(ok, "write_owned() under dryrun reports success even for an existing file");
    unsetenv("DANKC_THEME_DRYRUN");
    char *after = read_file(path);
    CHECK(before && after && strcmp(before, after) == 0,
          "write_owned() under DANKC_THEME_DRYRUN=1 never touches an existing file's content");
    free(before);
    free(after);

    scratch_clean(path);
}

/* --- dc_systheme_ensure_line() under DRYRUN: writes/backs up nothing -------- */

static void test_ensure_line_dryrun(void)
{
    char path[560];
    scratch_path(path, sizeof(path), "dryrun_ensure.css");
    scratch_clean(path);

    const char *needle = "@import 'dank-colors.css';";
    const char *marker = "/* Added by DankC */";
    const char *original = ".bar { color: blue; }\n";
    write_file(path, original);

    setenv("DANKC_THEME_DRYRUN", "1", 1);
    bool ok = dc_systheme_ensure_line(path, needle, marker, needle);
    unsetenv("DANKC_THEME_DRYRUN");

    CHECK(ok, "ensure_line() under DANKC_THEME_DRYRUN=1 reports success");
    char *content = read_file(path);
    CHECK(content && strcmp(content, original) == 0,
          "ensure_line() under dryrun leaves the file byte-for-byte unchanged");
    CHECK(count_backups(path) == 0, "ensure_line() under dryrun creates no backup file");
    free(content);

    scratch_clean(path);
}

int main(void)
{
    scratch_setup();

    test_hex_formatting();
    test_contrast_pick();
    test_ensure_line();
    test_ensure_line_top();
    test_write_owned();
    test_ensure_line_dryrun();

    scratch_teardown();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
