/* keybinds.h — shared backend for niri keybind editing (docs/23-KEYBIND-
 * EDITING-PLAN.md sec.2, task KB-T1).
 *
 * Generalizes ui/keybinds_modal.c's tolerant, read-only KDL parser
 * (kb_parse_config -> kb_parse_file -> kb_scan_binds_blocks ->
 * kb_parse_one_bind, following `include "...";` recursively) into a real
 * service two callers share:
 *   - the cheat-sheet overlay (ui/keybinds_modal.c, KB-T4) for display, and
 *   - the new Settings > Keybinds tab (ui/settings.c, KB-T3) for CRUD.
 *
 * Same managed-include shape as services/niri_input.c/display.c: dankc owns
 * ~/.config/niri/dankc-binds.kdl wholesale (rewritten from the in-memory
 * managed-bind list on every save) and appends a single
 * `include "dankc-binds.kdl";` line to the user's real config.kdl the first
 * time persistence is used (config.kdl is backed up first, timestamped). A
 * bind's `source` field (the basename of the file it was actually read from)
 * is how callers tell a dankc-managed bind (source == "dankc-binds.kdl",
 * `managed` also mirrors this) apart from one the user hand-wrote in
 * config.kdl or pulled in via their own `include`.
 *
 * Unlike keybinds_modal.c's read path (which tokenizes the action into argv
 * for a human label), this service keeps each bind's action body VERBATIM
 * (trimmed, trailing `;` dropped) so a managed bind round-trips byte-stable:
 * load -> edit unrelated binds -> persist must not perturb bytes of binds
 * the UI didn't touch.
 *
 * NEW vs niri_input.c: dc_keybinds_persist() snapshots the managed fragment's
 * prior bytes before rewriting it. If the post-write `niri validate` fails,
 * the snapshot is restored and re-validated, and the failure is reported
 * distinctly (DC_KEYBINDS_VALIDATE_FAILED_ROLLED_BACK) so a bad edit can
 * never leave niri's config half-broken between saves -- niri_input.c/
 * display.c's fragments are simple enough that a bad value there just means
 * niri falls back to a default; a stray unbalanced-brace or unknown-action
 * bind can break niri's WHOLE config, hence the harder guarantee here.
 *
 * All filesystem writes and the `niri validate` spawn are gated by
 * $DANKC_BINDS_DRYRUN=1 (logs the intended fragment/include/validate instead
 * of touching anything), same convention as $DANKC_NIRI_DRYRUN/
 * $DANKC_DISPLAY_DRYRUN/$DANKC_THEME_DRYRUN. `config_dir_override` is NULL in
 * production (real ~/.config/niri); tests/verification pass a scratch dir so
 * this never touches the user's real niri config. No config.json keys are
 * involved -- the on-disk KDL fragment IS the persisted state.
 */
#ifndef DC_SERVICES_KEYBINDS_H
#define DC_SERVICES_KEYBINDS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define DC_KEYBIND_CHORD_MAX 64
#define DC_KEYBIND_ACTION_MAX 192
#define DC_KEYBIND_TITLE_MAX 96
#define DC_KEYBIND_PROPS_MAX 96
#define DC_KEYBIND_SOURCE_MAX 128

/* One `<chord> [props...] { <action>; }` unit inside some `binds { }` node. */
typedef struct dc_keybind {
    char chord[DC_KEYBIND_CHORD_MAX];   /* e.g. "Mod+Shift+E", as written */
    char action[DC_KEYBIND_ACTION_MAX]; /* raw body, trimmed, no trailing ';' */
    char title[DC_KEYBIND_TITLE_MAX];   /* hotkey-overlay-title value, "" if none */
    char props[DC_KEYBIND_PROPS_MAX];   /* other header props verbatim, e.g.
                                          * "repeat=false allow-when-locked=true",
                                          * "" if none */
    char source[DC_KEYBIND_SOURCE_MAX]; /* basename of the file this bind came
                                          * from, e.g. "config.kdl",
                                          * "dankc-binds.kdl" */
    bool managed;                       /* source == "dankc-binds.kdl" */
} dc_keybind;

/* Outcome of the most recent dc_keybinds_persist()'s `niri validate` call. */
typedef enum {
    DC_KEYBINDS_VALIDATE_UNKNOWN = 0, /* dry-run, or niri/validate unavailable */
    DC_KEYBINDS_VALIDATE_OK,
    DC_KEYBINDS_VALIDATE_FAILED,               /* validation failed; NOT rolled back
                                                 * (write/rollback I/O itself failed --
                                                 * see the warn log for detail) */
    DC_KEYBINDS_VALIDATE_FAILED_ROLLED_BACK,   /* validation failed; the managed
                                                 * fragment was restored to its
                                                 * pre-persist bytes */
} dc_keybinds_validate_result;

/* A named niri-verb or `dankc ctl` preset action, for the Settings UI's
 * action picker (and the cheat-sheet's category/label prettification). */
typedef struct dc_keybind_action_preset {
    const char *verb;  /* niri table: the bare niri action verb, e.g.
                         * "close-window". dankc table: the `dankc ctl` word(s),
                         * e.g. "screenshot-region". */
    const char *label; /* human-readable label, e.g. "Close Window" */
    const char *cat;   /* category for grouping, e.g. "Window" */
} dc_keybind_action_preset;

/* Loads every bind reachable from `config_dir_override`'s (NULL: real
 * ~/.config/niri) config.kdl, following `include "...";` recursively (depth-
 * and visited-set-bounded, tolerates cycles/typos), into `out` (capacity
 * `max`). Returns the number of binds written to `out` (never more than
 * `max`; extra binds beyond `max` are silently dropped, matching
 * keybinds_modal.c's DC_KB_MAX_BINDS behavior). Returns 0 (with a warn log)
 * if $HOME is unset and no override was given. */
int dc_keybinds_load(dc_keybind *out, int max, const char *config_dir_override);

/* Rewrites ~/.config/niri/dankc-binds.kdl (or `config_dir_override`'s copy)
 * wholesale from `managed[0..n)` and ensures config.kdl includes it (backing
 * up config.kdl on first use, same as niri_input.c/display.c). Snapshots the
 * fragment's prior bytes first; if the post-write `niri validate` reports a
 * problem, the snapshot is restored and re-validated, and
 * dc_keybinds_last_validate() is left at
 * DC_KEYBINDS_VALIDATE_FAILED_ROLLED_BACK. Returns false only on a hard I/O
 * failure (couldn't write the managed file, ensure the include, or -- in the
 * rollback path -- restore the snapshot); a failed-but-rolled-back validate
 * still returns true (see dc_keybinds_last_validate() for that outcome). */
bool dc_keybinds_persist(const dc_keybind *managed, int n, const char *config_dir_override);

/* Outcome of the most recent dc_keybinds_persist() call's validation. */
dc_keybinds_validate_result dc_keybinds_last_validate(void);

/* Rewrites `in` (a "+"-joined key chord, e.g. "Shift+Mod+e") into `out` in a
 * canonical form: modifiers reordered to a fixed priority (Mod, Super, Ctrl,
 * Alt, Shift, ISO_Level3Shift, ISO_Level5Shift, then any unrecognized token
 * kept in its original relative order), and the trailing base-key token
 * resolved case-insensitively via xkb_keysym_from_name(...,
 * XKB_KEYSYM_CASE_INSENSITIVE) + xkb_keysym_get_name() so e.g. "Mod+Shift+e"
 * and "Shift+Mod+E" normalize identically. Unresolvable base-key tokens are
 * kept verbatim. Always NUL-terminates `out` (truncating if needed). */
void dc_keybinds_normalize_chord(const char *in, char *out, size_t n);

/* Normalized-compare `chord` against every bind in `all[0..n)` except index
 * `ignore_idx` (pass -1 to check all). Returns the index of the first
 * conflicting bind, or -1 if none. */
int dc_keybinds_find_conflict(const dc_keybind *all, int n, const char *chord, int ignore_idx);

/* Builds a chord string from a captured key-press: `base_keysym` is the
 * LEVEL-0 keysym of the physical key (so Shift+2 records "2", not the
 * shifted symbol), and `super`/`ctrl`/`alt`/`shift` are the modifiers held.
 * `super` is spelled "Mod" (niri's binds use "Mod" for the primary modifier,
 * conventionally Super). Returns false (leaving `out` untouched) if
 * `base_keysym` is itself a modifier keysym (Shift_L/R, Control_L/R,
 * Alt_L/R, Super_L/R, Meta_L/R, Hyper_L/R, Caps_Lock, Num_Lock,
 * ISO_Level3_Shift, ISO_Level5_Shift) or otherwise invalid/unresolvable. */
bool dc_keybinds_chord_from_capture(uint32_t base_keysym, bool super, bool ctrl, bool alt,
                                    bool shift, char *out, size_t n);

/* The niri-verb action preset table (moved from ui/keybinds_modal.c's
 * KB_NIRI_ACTIONS) and a new `dankc ctl` preset table (seeded from
 * ipc/control.c's control_dispatch() vocabulary), for the Settings UI's
 * 3-way action picker. `*count` receives the table's element count; the
 * returned pointer is a static, read-only array valid for the process
 * lifetime. */
const dc_keybind_action_preset *dc_keybinds_niri_actions(int *count);
const dc_keybind_action_preset *dc_keybinds_dankc_actions(int *count);

#endif /* DC_SERVICES_KEYBINDS_H */
