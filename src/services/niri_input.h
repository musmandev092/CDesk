/* niri_input.h — Mouse/Touchpad/Keyboard settings, written as a niri KDL
 * fragment (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.7).
 *
 * Same managed-include shape as services/display.c's dc_display_persist():
 * a dankc-owned file (~/.config/niri/dankc-input.kdl) rewritten wholesale
 * from the in-memory settings every time the Settings UI changes something,
 * plus a single `include "dankc-input.kdl"` line appended to the user's real
 * config.kdl the first time persistence is used (backed up first). dankc
 * never otherwise touches config.kdl. This is an independent copy of that
 * write+ensure-include logic (not a shared helper) -- same file-ownership
 * boundary rationale as ui/settings.c's niri Window Rules editor.
 *
 * niri's `input {}` KDL block uses bare-keyword presence for booleans (e.g.
 * `tap`, `natural-scroll`, `dwt`) rather than `key true/false` -- there is no
 * KDL spelling for "explicitly off", only "present" (on) or "absent"
 * (niri's own default). So every bool below means "emit this keyword",
 * not a tri-state.
 *
 * After writing, `niri validate -c <config.kdl>` is run synchronously (this
 * needs an actual waitpid()-able child, so it temporarily restores default
 * SIGCHLD handling around the fork -- main.c's process-wide
 * `signal(SIGCHLD, SIG_IGN)` would otherwise make waitpid() fail with
 * ECHILD, see printers.c's dc_printers_available() comment for the general
 * problem) so a malformed fragment is caught and logged immediately rather
 * than silently breaking the user's next niri restart. Gated by
 * $DANKC_NIRI_DRYRUN=1 (logs the fragment content + intended path/validate
 * command instead of touching any file or spawning niri), same convention
 * as $DANKC_DISPLAY_DRYRUN.
 */
#ifndef DC_SERVICES_NIRI_INPUT_H
#define DC_SERVICES_NIRI_INPUT_H

#include <stdbool.h>

typedef struct dc_niri_input_config {
    bool touchpad_tap;
    bool touchpad_natural_scroll;
    bool touchpad_dwt; /* disable-while-typing */
    bool touchpad_disabled_on_external_mouse;
    bool touchpad_accel_enabled; /* whether to emit accel-speed at all */
    float touchpad_accel_speed;  /* -1..1, only written if touchpad_accel_enabled */

    bool mouse_natural_scroll;
    bool mouse_accel_enabled;
    float mouse_accel_speed; /* -1..1, only written if mouse_accel_enabled */

    bool keyboard_numlock;
    const char *keyboard_layout; /* xkb layout string e.g. "us"; NULL/empty = unset */
} dc_niri_input_config;

/* Result of the post-write `niri validate` (best-effort; a failed/unknown
 * validation does NOT undo the write -- see the .c file's rationale). */
typedef enum {
    DC_NIRI_INPUT_VALIDATE_UNKNOWN = 0, /* dry-run, or niri/validate unavailable */
    DC_NIRI_INPUT_VALIDATE_OK,
    DC_NIRI_INPUT_VALIDATE_FAILED,
} dc_niri_input_validate_result;

/* Writes ~/.config/niri/dankc-input.kdl from `cfg` and ensures config.kdl
 * includes it (backing up config.kdl on first use, same as display.c).
 * `config_dir_override` is NULL in production; tests pass a scratch dir so
 * this never touches ~/.config/niri. Returns false on a hard I/O failure
 * (couldn't write the managed file or ensure the include). See
 * dc_niri_input_last_validate() for the post-write validation outcome. */
bool dc_niri_input_persist(const dc_niri_input_config *cfg, const char *config_dir_override);

/* Outcome of the most recent dc_niri_input_persist()'s `niri validate` call,
 * for the Settings UI to surface as a hint. */
dc_niri_input_validate_result dc_niri_input_last_validate(void);

#endif /* DC_SERVICES_NIRI_INPUT_H */
