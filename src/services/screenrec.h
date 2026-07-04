/* screenrec.h — screen recording service (docs/29-SMALL-FEATURES-PLAN.md
 * sec.5): fork+exec a Wayland screen recorder (wl-screenrec preferred,
 * wf-recorder fallback) full-screen or over an interactively-selected
 * region, track the single in-flight recording, and stop it cleanly.
 *
 * Ownership model: module-static single-recording state (dankc only ever
 * drives one recording at a time, matching the bar widget planned in a later
 * task). No config/persistence here -- `screenRecorderCmd` override and the
 * bar widget/IPC verbs land in later tasks (see docs/29 sec.5 "Tasks").
 *
 * Process model: SIGCHLD is SIG_IGN process-wide (main.c), so there is no
 * waitpid() anywhere in this codebase -- liveness is polled via
 * kill(pid, 0) (see dc_screenrec_active(), meant to be called from the 1 Hz
 * clock_tick in main.c by a later task) rather than a SIGCHLD handler.
 *
 * Full-screen recording: fork()+execlp the recorder directly, so the forked
 * pid *is* the recorder's pid.
 *
 * Region recording: slurp's interactive selection has to run and produce a
 * geometry string *before* the recorder can be launched with it, and this
 * service doesn't want a synchronous wait (that would block the compositor
 * event loop while the user drags a selection box). So instead it forks a
 * `/bin/sh -c 'g=$(slurp) && exec <recorder> -g "$g" -f <path>'` -- the
 * `exec` is what makes the forked pid become the recorder's pid once slurp
 * resolves (a plain `sh -c '... && recorder ...'` without exec would leave
 * the tracked pid as the shell, which signals wrong and outlives the
 * recorder). If the user cancels slurp (Esc), the shell's `&&` short-circuits
 * and the whole child exits quickly with no output file -- see
 * dc_screenrec_active()'s "died <2s with no file" clean-cancel handling
 * below, which swallows that case instead of surfacing it as an error.
 */
#ifndef DC_SERVICES_SCREENREC_H
#define DC_SERVICES_SCREENREC_H

#include <stdbool.h>

struct dc_notifications;

/* Start a new recording. `region` selects an interactive slurp selection
 * instead of the full screen. Builds the output path as
 * ${XDG_VIDEOS_DIR:-$HOME/Videos}/recording-YYYYmmdd-HHMMSS.mp4 (creating the
 * directory as needed), probes for wl-screenrec (preferred) or wf-recorder on
 * PATH, and launches it detached.
 *
 * Returns false (and, if `n` is non-NULL, posts a "recorder not found"
 * notification) when neither recorder binary is on PATH, a recording is
 * already active, or the output directory couldn't be created. `n` may be
 * NULL to suppress the failure notification (e.g. for a dry probe). */
bool dc_screenrec_start(struct dc_notifications *n, bool region);

/* Stop the active recording (kill(pid, SIGINT) -- never SIGKILL, so
 * wl-screenrec/wf-recorder get to finalize/mux the mp4 container cleanly),
 * post a "Screen recording" / "Saved <path>" notification via `n` (may be
 * NULL to suppress it), and clear the tracked state. No-op if nothing is
 * currently recording. */
void dc_screenrec_stop(struct dc_notifications *n);

/* True if a recording is currently in flight. Polls kill(pid, 0): if the
 * tracked child has died on its own (recorder crashed, or -- for a region
 * recording -- slurp was cancelled before ever exec-ing into the recorder),
 * clears the state and returns false. A death within 2 seconds of start with
 * no output file yet on disk is treated as a clean user cancel (no
 * notification, no error log); any other unexpected death is logged. Meant
 * to be polled once per second from main.c's clock_tick. */
bool dc_screenrec_active(void);

/* Seconds elapsed since the active recording started (0 if none is active).
 * For the future bar-widget elapsed-time readout. */
int dc_screenrec_elapsed_sec(void);

#endif /* DC_SERVICES_SCREENREC_H */
