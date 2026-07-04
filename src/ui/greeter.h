/* greeter.h — the greetd login greeter UI surface (docs/28-GREETER-PLAN.md T3).
 *
 * Visually this borrows lock.c's per-output surface/scale/EGL/render
 * plumbing and clock/date/password-pill draw code, but structurally it is a
 * fullscreen OVERLAY wlr-layer-shell surface per output (powermenu.c's
 * pattern: get_layer_surface + anchor all 4 + set_size(0,0) + exclusive
 * keyboard interactivity), NOT an ext-session-lock surface — the greeter
 * runs standalone (via `dankc greeter`, wired up in T4) before any normal
 * session exists, so there is no compositor session-lock protocol to grab.
 *
 * Unlike lock.c/powermenu.c (which authenticate against PAM or exist only
 * while an already-running shell is up), this module speaks the greetd wire
 * protocol (services/greetd.h) to authenticate a user and hand off to their
 * chosen desktop session. It owns no D-Bus/PAM/niri-IPC state — T4's reduced
 * init intentionally does not start any of that for the greeter process.
 *
 * Input routing: keysyms are delivered globally (dc_wayland has one
 * process-wide keyboard focus, unlike per-surface click/motion), so
 * dc_greeter_handle_key() takes no surface. Click/motion carry the
 * `wl_surface` they landed on because the greeter draws the same card
 * replicated across every connected output (multi-monitor), and the
 * function must first work out which output's logical coordinate space
 * `x`/`y` are in.
 */
#ifndef DC_UI_GREETER_H
#define DC_UI_GREETER_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_loop;
struct wl_surface;

typedef struct dc_greeter dc_greeter;

/* Invoked exactly once, after start_session's greetd response confirms the
 * chosen session actually launched (see greeter.c's state machine: STARTING
 * -> DONE). greetd then waits for this process to exit before handing the
 * seat to the started session, so the callback's job is to make the
 * greeter's own event loop return -- T4's dc_greeter_main() passes
 * dc_loop_stop() (or an equivalent wrapper that also _exit(0)s). Never
 * invoked more than once per dc_greeter. */
typedef void (*dc_greeter_done_cb)(void *user_data);

/* Create the greeter: enumerates local users/sessions (services/
 * greeter_data.h), connects to greetd (services/greetd.h; a failed
 * connection is logged but not fatal -- the UI still comes up and shows
 * "greetd unavailable" rather than crashing, so a misconfigured/missing
 * $GREETD_SOCK is visible instead of a blank screen), and opens one
 * fullscreen overlay layer-shell surface per currently-connected output.
 * Never returns NULL (matches lock.c/powermenu.c's create() convention:
 * partial failures degrade rather than abort). */
dc_greeter *dc_greeter_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render,
                              struct dc_loop *loop, dc_greeter_done_cb done_cb, void *user_data);

/* Tear down every per-output surface, close the greetd connection, and free
 * all state. Safe to call with NULL. */
void dc_greeter_destroy(dc_greeter *g);

/* Keyboard input: Up/Down/Tab move the user-picker selection, Left/Right
 * cycle the session picker, Enter advances the state machine (create the
 * greetd session / submit the current password), Backspace/printable text
 * edit the password field while a prompt is open, Escape cancels back to the
 * user picker. See greeter.c's top comment for the full state machine. */
void dc_greeter_handle_key(dc_greeter *g, uint32_t keysym, const char *utf8);

/* Left-click at logical (x, y) on `surface` (one of this greeter's own
 * per-output surfaces -- clicks on any other surface are ignored): selects
 * (and, from the user list, immediately submits) a user row, or cycles the
 * session picker via its chevrons. No-op if `surface` isn't one of ours. */
void dc_greeter_handle_click(dc_greeter *g, struct wl_surface *surface, double x, double y);

/* Pointer motion at logical (x, y) on `surface`: moves the user-row hover
 * highlight. Purely cosmetic -- never changes the state machine. */
void dc_greeter_handle_motion(dc_greeter *g, struct wl_surface *surface, double x, double y);

#endif /* DC_UI_GREETER_H */
