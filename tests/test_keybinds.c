/* Standalone unit test for src/services/keybinds.c (docs/23-KEYBIND-EDITING-
 * PLAN.md sec.2, task KB-T1) -- the generalized KDL bind parser, chord
 * normalization/conflict detection/capture, and the managed-fragment
 * persist+include+backup+validate+rollback pipeline.
 *
 * keybinds.c only references core/log.c (dc_warn/dc_info/dc_debug/dc_error)
 * plus libc and libxkbcommon -- unlike systheme.c/text_edit.c, there is no
 * EGL/nanovg/theme surface it also drags in, so this test links the real
 * translation unit directly with no link-only stub file needed (see
 * tests/test_systheme_stubs.c for why THAT one does).
 *
 * Every filesystem-touching test uses a scratch directory (mkdtemp'd under
 * $TMPDIR/or /tmp) as `config_dir_override` -- this suite must NEVER read or
 * write the real ~/.config/niri. The rollback test additionally shells out
 * to the real `niri validate` (skipped with a note if `niri` isn't on
 * $PATH), since dc_keybinds_persist()'s rollback path only fires on an
 * actual validation failure -- see run_niri_validate()'s SIGCHLD dance in
 * keybinds.c.
 */
#include "services/keybinds.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>

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

/* --- scratch-dir plumbing (same pattern as tests/test_systheme.c) --------- */

static char g_scratch_dir[512];

static void scratch_setup(void)
{
    snprintf(g_scratch_dir, sizeof(g_scratch_dir), "/tmp/dankc_keybinds_test_XXXXXX");
    if (!mkdtemp(g_scratch_dir)) {
        fprintf(stderr, "FATAL: mkdtemp failed: %s\n", strerror(errno));
        exit(1);
    }
}

static void scratch_path(char *out, size_t cap, const char *name)
{
    snprintf(out, cap, "%s/%s", g_scratch_dir, name);
}

static void write_file(const char *path, const char *content)
{
    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "FATAL: could not write fixture %s: %s\n", path, strerror(errno));
        exit(1);
    }
    fputs(content, f);
    fclose(f);
}

static char *read_file_alloc(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    if (sz < 0) {
        fclose(f);
        return NULL;
    }
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

static bool file_exists(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

/* Count how many `<scratch>/config.kdl.bak-*` backup files exist. */
static int count_config_backups(void)
{
    char pattern[600];
    scratch_path(pattern, sizeof(pattern), "config.kdl.bak-");
    strcat(pattern, "*");
    /* Avoid pulling in <glob.h> for one call: just check a handful of
     * plausible epoch-second neighbors is overkill -- shell out instead,
     * this is a test, not production code. */
    char cmd[700];
    snprintf(cmd, sizeof(cmd), "ls %s 2>/dev/null | wc -l", pattern);
    FILE *p = popen(cmd, "r");
    if (!p)
        return -1;
    int n = -1;
    if (fscanf(p, "%d", &n) != 1)
        n = -1;
    pclose(p);
    return n;
}

static bool niri_available(void)
{
    return system("niri --version >/dev/null 2>&1") == 0;
}

/* --- parse fixtures -------------------------------------------------------- */

static void test_parse_fixtures(void)
{
    scratch_setup();

    char config_path[600], extra_path[600], managed_path[600];
    scratch_path(config_path, sizeof(config_path), "config.kdl");
    scratch_path(extra_path, sizeof(extra_path), "extra.kdl");
    scratch_path(managed_path, sizeof(managed_path), "dankc-binds.kdl");

    /* config.kdl: a top-level binds{} block with a titled bind + props, plus
     * two includes (one ordinary user file, one the "managed" filename). */
    write_file(config_path,
               "// a comment that should be stripped\n"
               "input {\n"
               "}\n"
               "binds {\n"
               "    Mod+Shift+E hotkey-overlay-title=\"Quit DankC\" repeat=false {\n"
               "        spawn \"dankc\" \"ctl\" \"quit\";\n"
               "    }\n"
               "}\n"
               "include \"extra.kdl\";\n"
               "include \"dankc-binds.kdl\";\n");

    write_file(extra_path,
               "binds {\n"
               "    Mod+Return { spawn \"foo\" \"bar baz\"; }\n"
               "}\n");

    write_file(managed_path,
               "// Managed by DankC's Settings > Keybinds tab.\n"
               "binds {\n"
               "    Mod+D { spawn \"dankc\" \"ctl\" \"launcher\"; }\n"
               "}\n");

    dc_keybind binds[16];
    int n = dc_keybinds_load(binds, 16, g_scratch_dir);
    CHECK(n == 3, "loaded exactly 3 binds across config.kdl + 2 includes (got %d)", n);

    /* Find each bind by chord (load order across binds{} blocks/files is an
     * implementation detail we don't want to pin down). */
    const dc_keybind *quit = NULL, *ret = NULL, *launcher = NULL;
    for (int i = 0; i < n; i++) {
        if (strcmp(binds[i].chord, "Mod+Shift+E") == 0)
            quit = &binds[i];
        else if (strcmp(binds[i].chord, "Mod+Return") == 0)
            ret = &binds[i];
        else if (strcmp(binds[i].chord, "Mod+D") == 0)
            launcher = &binds[i];
    }

    CHECK(quit != NULL, "found the Mod+Shift+E bind");
    if (quit) {
        CHECK(strcmp(quit->title, "Quit DankC") == 0, "hotkey-overlay-title extracted (got \"%s\")",
              quit->title);
        CHECK(strstr(quit->props, "repeat=false") != NULL, "props preserved repeat=false (got \"%s\")",
              quit->props);
        CHECK(strcmp(quit->action, "spawn \"dankc\" \"ctl\" \"quit\"") == 0,
              "action body verbatim (got \"%s\")", quit->action);
        CHECK(strcmp(quit->source, "config.kdl") == 0, "source is config.kdl (got \"%s\")",
              quit->source);
        CHECK(quit->managed == false, "bind from config.kdl is not managed");
    }

    CHECK(ret != NULL, "found the Mod+Return bind from the included extra.kdl");
    if (ret) {
        CHECK(strcmp(ret->action, "spawn \"foo\" \"bar baz\"") == 0,
              "quoted-arg action preserved verbatim (got \"%s\")", ret->action);
        CHECK(strcmp(ret->source, "extra.kdl") == 0, "source is extra.kdl (got \"%s\")", ret->source);
        CHECK(ret->managed == false, "bind from extra.kdl is not managed");
    }

    CHECK(launcher != NULL, "found the Mod+D bind from dankc-binds.kdl");
    if (launcher) {
        CHECK(launcher->managed == true, "bind sourced from dankc-binds.kdl is managed");
        CHECK(strcmp(launcher->source, "dankc-binds.kdl") == 0, "source is dankc-binds.kdl (got \"%s\")",
              launcher->source);
    }
}

/* --- normalize -------------------------------------------------------------- */

static void test_normalize(void)
{
    char a[DC_KEYBIND_CHORD_MAX], b[DC_KEYBIND_CHORD_MAX];

    dc_keybinds_normalize_chord("Shift+Mod+e", a, sizeof(a));
    dc_keybinds_normalize_chord("Mod+Shift+E", b, sizeof(b));
    CHECK(a[0] && strcmp(a, b) == 0, "\"Shift+Mod+e\" normalizes == \"Mod+Shift+E\" (\"%s\" vs \"%s\")",
          a, b);

    dc_keybinds_normalize_chord("Alt+Ctrl+Delete", a, sizeof(a));
    dc_keybinds_normalize_chord("Ctrl+Alt+Delete", b, sizeof(b));
    CHECK(strcmp(a, b) == 0, "\"Alt+Ctrl+Delete\" normalizes == \"Ctrl+Alt+Delete\" (\"%s\" vs \"%s\")",
          a, b);

    /* Known modifiers always sort ahead of an unrecognized token, regardless
     * of the order they were typed in. */
    dc_keybinds_normalize_chord("Mod+Weird+x", a, sizeof(a));
    dc_keybinds_normalize_chord("Weird+Mod+x", b, sizeof(b));
    CHECK(strcmp(a, b) == 0,
          "unrecognized modifier token still sorts after known ones either way (\"%s\" vs \"%s\")", a,
          b);

    dc_keybinds_normalize_chord("mod+shift+e", a, sizeof(a));
    dc_keybinds_normalize_chord("Mod+Shift+E", b, sizeof(b));
    CHECK(strcmp(a, b) == 0, "\"mod+shift+e\" normalizes == \"Mod+Shift+E\" (\"%s\" vs \"%s\")", a, b);

    char empty[DC_KEYBIND_CHORD_MAX];
    dc_keybinds_normalize_chord("", empty, sizeof(empty));
    CHECK(empty[0] == '\0', "normalizing an empty chord yields an empty string");
}

/* --- conflict detection ----------------------------------------------------- */

static void test_conflict(void)
{
    dc_keybind all[4] = {0};
    snprintf(all[0].chord, sizeof(all[0].chord), "Mod+D");
    snprintf(all[1].chord, sizeof(all[1].chord), "Shift+Mod+E");
    snprintf(all[2].chord, sizeof(all[2].chord), "Mod+Return");
    snprintf(all[3].chord, sizeof(all[3].chord), "Mod+Q");

    int idx = dc_keybinds_find_conflict(all, 4, "Mod+Shift+E", -1);
    CHECK(idx == 1, "\"Mod+Shift+E\" conflicts with all[1]'s \"Shift+Mod+E\" (got idx=%d)", idx);

    idx = dc_keybinds_find_conflict(all, 4, "Mod+Shift+E", 1);
    CHECK(idx == -1, "conflict check ignores its own index (ignore_idx=1) (got idx=%d)", idx);

    idx = dc_keybinds_find_conflict(all, 4, "Mod+Shift+Z", -1);
    CHECK(idx == -1, "an unused chord reports no conflict (got idx=%d)", idx);
}

/* --- capture ---------------------------------------------------------------- */

static void test_chord_from_capture(void)
{
    char out[DC_KEYBIND_CHORD_MAX];

    bool ok = dc_keybinds_chord_from_capture(XKB_KEY_e, true, false, false, true, out, sizeof(out));
    CHECK(ok && strcmp(out, "Mod+Shift+e") == 0, "Super+Shift+e capture -> \"Mod+Shift+e\" (got \"%s\")",
          ok ? out : "(false)");

    ok = dc_keybinds_chord_from_capture(XKB_KEY_F5, false, true, false, false, out, sizeof(out));
    CHECK(ok && strcmp(out, "Ctrl+F5") == 0, "Ctrl+F5 capture (got \"%s\")", ok ? out : "(false)");

    ok = dc_keybinds_chord_from_capture(XKB_KEY_Shift_L, true, false, false, false, out, sizeof(out));
    CHECK(!ok, "capturing a bare modifier keysym (Shift_L) is rejected");

    ok = dc_keybinds_chord_from_capture(XKB_KEY_Super_L, false, false, false, false, out, sizeof(out));
    CHECK(!ok, "capturing a bare Super_L keysym is rejected");

    ok = dc_keybinds_chord_from_capture(XKB_KEY_NoSymbol, true, false, false, false, out, sizeof(out));
    CHECK(!ok, "capturing XKB_KEY_NoSymbol is rejected");
}

/* --- persist round-trip ------------------------------------------------------ */

static void test_persist_roundtrip(void)
{
    scratch_setup();
    unsetenv("DANKC_BINDS_DRYRUN");

    char config_path[600], managed_path[600];
    scratch_path(config_path, sizeof(config_path), "config.kdl");
    scratch_path(managed_path, sizeof(managed_path), "dankc-binds.kdl");

    /* Pre-seed a real user config.kdl so ensure_include() has to append +
     * back up (rather than create-from-scratch). */
    write_file(config_path, "input {\n}\n");

    dc_keybind managed[2] = {0};
    snprintf(managed[0].chord, sizeof(managed[0].chord), "Mod+D");
    snprintf(managed[0].action, sizeof(managed[0].action), "spawn \"dankc\" \"ctl\" \"launcher\"");
    snprintf(managed[0].title, sizeof(managed[0].title), "App Launcher");

    snprintf(managed[1].chord, sizeof(managed[1].chord), "Mod+N");
    snprintf(managed[1].action, sizeof(managed[1].action), "spawn \"dankc\" \"ctl\" \"notifications\"");

    bool ok = dc_keybinds_persist(managed, 2, g_scratch_dir);
    CHECK(ok, "dc_keybinds_persist() returns true");

    char *frag = read_file_alloc(managed_path);
    CHECK(frag != NULL, "managed fragment %s was written", managed_path);
    if (frag) {
        CHECK(strstr(frag, "Mod+D") != NULL, "fragment contains the Mod+D bind");
        CHECK(strstr(frag, "hotkey-overlay-title=\"App Launcher\"") != NULL,
              "fragment contains the escaped title");
        CHECK(strstr(frag, "spawn \"dankc\" \"ctl\" \"notifications\"") != NULL,
              "fragment contains the Mod+N action verbatim");
        CHECK(strstr(frag, "Managed by DankC") != NULL, "fragment carries the managed-file header");
        free(frag);
    }

    char *cfg = read_file_alloc(config_path);
    CHECK(cfg != NULL, "config.kdl still exists");
    if (cfg) {
        CHECK(strstr(cfg, "include \"dankc-binds.kdl\"") != NULL,
              "config.kdl got the include line appended");
        free(cfg);
    }

    int backups = count_config_backups();
    CHECK(backups == 1, "exactly one config.kdl backup was made on first include (got %d)", backups);

    /* Persist again with a changed set -- the include is already present, so
     * ensure_include() must be a no-op (no second backup), while the
     * fragment itself is still rewritten wholesale. */
    dc_keybind managed2[1] = {0};
    snprintf(managed2[0].chord, sizeof(managed2[0].chord), "Mod+V");
    snprintf(managed2[0].action, sizeof(managed2[0].action), "spawn \"dankc\" \"ctl\" \"clipboard\"");

    ok = dc_keybinds_persist(managed2, 1, g_scratch_dir);
    CHECK(ok, "second dc_keybinds_persist() returns true");

    backups = count_config_backups();
    CHECK(backups == 1, "still exactly one config.kdl backup after a second persist (got %d)", backups);

    frag = read_file_alloc(managed_path);
    if (frag) {
        CHECK(strstr(frag, "Mod+V") != NULL, "second fragment contains the new Mod+V bind");
        CHECK(strstr(frag, "Mod+D") == NULL,
              "second fragment no longer contains the first save's Mod+D (wholesale rewrite)");
        free(frag);
    }

    /* Round-trip: load what we just persisted back and check it matches. */
    dc_keybind loaded[8];
    int n = dc_keybinds_load(loaded, 8, g_scratch_dir);
    CHECK(n == 1, "load-back after second persist sees exactly 1 bind (got %d)", n);
    if (n == 1) {
        CHECK(strcmp(loaded[0].chord, "Mod+V") == 0, "loaded chord round-trips (got \"%s\")",
              loaded[0].chord);
        CHECK(strcmp(loaded[0].action, "spawn \"dankc\" \"ctl\" \"clipboard\"") == 0,
              "loaded action round-trips byte-stable (got \"%s\")", loaded[0].action);
        CHECK(loaded[0].managed == true, "loaded bind is flagged managed");
    }

    if (niri_available()) {
        CHECK(dc_keybinds_last_validate() == DC_KEYBINDS_VALIDATE_OK,
              "niri validate reports OK for a well-formed persisted config (got %d)",
              (int)dc_keybinds_last_validate());
    } else {
        printf("skip: niri not on PATH, not asserting dc_keybinds_last_validate() == OK\n");
    }
}

/* --- dry-run gate ------------------------------------------------------------ */

static void test_dryrun_gate(void)
{
    scratch_setup();

    char config_path[600], managed_path[600];
    scratch_path(config_path, sizeof(config_path), "config.kdl");
    scratch_path(managed_path, sizeof(managed_path), "dankc-binds.kdl");
    write_file(config_path, "input {\n}\n");

    setenv("DANKC_BINDS_DRYRUN", "1", 1);

    dc_keybind managed[1] = {0};
    snprintf(managed[0].chord, sizeof(managed[0].chord), "Mod+X");
    snprintf(managed[0].action, sizeof(managed[0].action), "spawn \"true\"");

    bool ok = dc_keybinds_persist(managed, 1, g_scratch_dir);
    CHECK(ok, "dc_keybinds_persist() under DANKC_BINDS_DRYRUN=1 still returns true");
    CHECK(!file_exists(managed_path), "dry-run does NOT write the managed fragment");

    char *cfg = read_file_alloc(config_path);
    CHECK(cfg && strstr(cfg, "dankc-binds.kdl") == NULL,
          "dry-run does NOT touch config.kdl's include line");
    free(cfg);

    unsetenv("DANKC_BINDS_DRYRUN");
}

/* --- rollback ----------------------------------------------------------------- */

static void test_rollback(void)
{
    if (!niri_available()) {
        printf("skip: `niri` not found on PATH, cannot exercise the real validate-failure/rollback "
               "path\n");
        return;
    }

    scratch_setup();
    unsetenv("DANKC_BINDS_DRYRUN");

    char config_path[600], managed_path[600];
    scratch_path(config_path, sizeof(config_path), "config.kdl");
    scratch_path(managed_path, sizeof(managed_path), "dankc-binds.kdl");
    write_file(config_path, "input {\n}\n");

    /* First, a known-good persist -- this is the state a rolled-back save
     * must return to. */
    dc_keybind good[1] = {0};
    snprintf(good[0].chord, sizeof(good[0].chord), "Mod+D");
    snprintf(good[0].action, sizeof(good[0].action), "spawn \"dankc\" \"ctl\" \"launcher\"");
    bool ok = dc_keybinds_persist(good, 1, g_scratch_dir);
    CHECK(ok, "seeding a good persist before the rollback test succeeds");
    CHECK(dc_keybinds_last_validate() == DC_KEYBINDS_VALIDATE_OK, "seed persist validates OK (got %d)",
          (int)dc_keybinds_last_validate());

    char *good_bytes = read_file_alloc(managed_path);
    CHECK(good_bytes != NULL, "captured the good fragment's bytes for comparison");

    /* Now persist a deliberately bogus action -- niri validate rejects
     * unknown action verbs (verified manually: "expected `quit`, `suspend`,
     * or one of 133 others"), which must trigger the rollback path. */
    dc_keybind bad[1] = {0};
    snprintf(bad[0].chord, sizeof(bad[0].chord), "Mod+Z");
    snprintf(bad[0].action, sizeof(bad[0].action), "totally-bogus-action-xyz");
    ok = dc_keybinds_persist(bad, 1, g_scratch_dir);
    CHECK(ok, "dc_keybinds_persist() still returns true when it rolls back (I/O itself succeeded)");
    CHECK(dc_keybinds_last_validate() == DC_KEYBINDS_VALIDATE_FAILED_ROLLED_BACK,
          "last_validate reports FAILED_ROLLED_BACK (got %d)", (int)dc_keybinds_last_validate());

    char *after_bytes = read_file_alloc(managed_path);
    CHECK(after_bytes != NULL, "managed fragment still exists after rollback");
    if (good_bytes && after_bytes) {
        CHECK(strcmp(good_bytes, after_bytes) == 0,
              "managed fragment bytes were restored to the pre-persist snapshot");
    }

    /* Confirm via the parser too: the bogus bind must NOT be visible, the
     * good one must still be. */
    dc_keybind loaded[8];
    int n = dc_keybinds_load(loaded, 8, g_scratch_dir);
    bool saw_good = false, saw_bad = false;
    for (int i = 0; i < n; i++) {
        if (strcmp(loaded[i].chord, "Mod+D") == 0)
            saw_good = true;
        if (strcmp(loaded[i].chord, "Mod+Z") == 0)
            saw_bad = true;
    }
    CHECK(saw_good, "post-rollback load still sees the good Mod+D bind");
    CHECK(!saw_bad, "post-rollback load does NOT see the rolled-back Mod+Z bind");

    free(good_bytes);
    free(after_bytes);
}

/* --- action-preset tables ----------------------------------------------------- */

static void test_action_tables(void)
{
    int count = 0;
    const dc_keybind_action_preset *niri = dc_keybinds_niri_actions(&count);
    CHECK(niri != NULL && count > 0, "dc_keybinds_niri_actions() returns a non-empty table (%d)",
          count);

    int dcount = 0;
    const dc_keybind_action_preset *dankc = dc_keybinds_dankc_actions(&dcount);
    CHECK(dankc != NULL && dcount > 0, "dc_keybinds_dankc_actions() returns a non-empty table (%d)",
          dcount);

    bool found_launcher = false;
    for (int i = 0; i < dcount; i++) {
        if (strcmp(dankc[i].verb, "launcher") == 0)
            found_launcher = true;
    }
    CHECK(found_launcher, "dankc action table includes the \"launcher\" ctl command");
}

int main(void)
{
    test_parse_fixtures();
    test_normalize();
    test_conflict();
    test_chord_from_capture();
    test_persist_roundtrip();
    test_dryrun_gate();
    test_rollback();
    test_action_tables();

    printf("\n%d/%d checks passed\n", g_checks - g_failures, g_checks);
    return g_failures == 0 ? 0 : 1;
}
