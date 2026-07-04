/* greeter_data.h — greeter user/session enumeration + last-choice memory.
 *
 * The greeter (docs/28-GREETER-PLAN.md, T2) needs three unrelated bits of
 * data, none of which touch greetd's IPC wire protocol (services/greetd.h) at
 * all:
 *
 *   1. Which local accounts are real login users (dc_greeter_users), so the
 *      user picker doesn't list system/service accounts (getent-style
 *      filtering: uid in [1000,60000), a real shell, not "nobody").
 *   2. Which desktop sessions are installed (dc_greeter_sessions) — the same
 *      XDG wayland-sessions/xsessions .desktop scan every other greeter
 *      (gtkgreet, lightdm, sddm, ...) does.
 *   3. The last user/session picked, so the greeter can default to it next
 *      boot (dc_greeter_remember / dc_greeter_last_user / dc_greeter_last_session)
 *      — persisted as two one-line files under $DANKC_GREETER_STATE_DIR (the
 *      `dankc-greeter` wrapper script sets this to the greeter's cache dir;
 *      it is NOT resolved here, so this module has no XDG-cache-dir opinion
 *      of its own and is trivially testable by pointing the env var at a
 *      scratch directory).
 *
 * Every function here tolerates a missing/unreadable/unwritable environment
 * (no getpwent entries, no session dirs, no state dir) by returning 0/false
 * rather than erroring — the greeter should always be able to draw *something*
 * (docs/28-GREETER-PLAN.md risk: "fake-HOME must not crash").
 */
#ifndef DC_SERVICES_GREETER_DATA_H
#define DC_SERVICES_GREETER_DATA_H

#include <stdbool.h>
#include <stddef.h>

#define DC_GREETER_USER_NAME 64
#define DC_GREETER_USER_DISPLAY 96

#define DC_GREETER_SESSION_NAME 64
#define DC_GREETER_SESSION_EXEC 256
#define DC_GREETER_SESSION_DESKTOP_NAMES 128

typedef struct {
    char name[DC_GREETER_USER_NAME];       /* pw_name, e.g. "alice" */
    char display[DC_GREETER_USER_DISPLAY]; /* GECOS full-name field, falls back to name */
} dc_greeter_user;

typedef struct {
    char name[DC_GREETER_SESSION_NAME];                   /* Name= */
    char exec[DC_GREETER_SESSION_EXEC];                   /* Exec=, field codes stripped */
    char desktop_names[DC_GREETER_SESSION_DESKTOP_NAMES];  /* DesktopNames=, raw (may be empty) */
    bool is_x11;                                          /* found under xsessions/, not wayland-sessions/ */
} dc_greeter_session;

/* Enumerate real local login accounts via getpwent(): uid in [1000,60000),
 * pw_shell not ending in "nologin" or "false", pw_name != "nobody", pw_dir !=
 * "/var/empty". Writes up to `max` entries into `out` (order = passwd
 * database order) and returns the count written. `display` is the GECOS
 * field's first (comma-separated) part, falling back to pw_name if GECOS is
 * empty or has an empty first field. */
int dc_greeter_users(dc_greeter_user *out, int max);

/* Enumerate installed desktop sessions: scans $XDG_DATA_DIRS (or
 * "/usr/local/share:/usr/share" if unset/empty), each entry's
 * "wayland-sessions" subdirectory, then "xsessions" — so Wayland sessions
 * sort before X11 ones in `out`. Every *.desktop file's [Desktop Entry] group
 * is parsed for Name=/Exec=/DesktopNames=; entries with Hidden=true or
 * missing Name=/Exec= are skipped. Exec= has desktop-entry field codes
 * (%f/%u/%c/...) and surrounding quotes stripped (services/apps.c's
 * clean_exec() convention). Earlier data dirs win on a duplicate desktop-file
 * id, per the XDG data-dirs spec. Writes up to `max` entries into `out` and
 * returns the count written. */
int dc_greeter_sessions(dc_greeter_session *out, int max);

/* Persist `user`/`session_name` as the last successful picks, for
 * dc_greeter_last_user()/dc_greeter_last_session() to default to next time.
 * Either argument may be NULL to leave that half of the memory untouched. A
 * no-op (does not error or log) if $DANKC_GREETER_STATE_DIR is unset, empty,
 * or the directory can't be written to. */
void dc_greeter_remember(const char *user, const char *session_name);

/* Read back the last dc_greeter_remember()'d user/session name into `out`
 * (a buffer of `n` bytes, NUL-terminated). Returns true and fills `out` iff a
 * value was found; leaves `out` untouched and returns false if
 * $DANKC_GREETER_STATE_DIR is unset/empty, the file is missing/unreadable, or
 * it's empty. */
bool dc_greeter_last_user(char *out, size_t n);
bool dc_greeter_last_session(char *out, size_t n);

#endif /* DC_SERVICES_GREETER_DATA_H */
