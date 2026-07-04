/* notepad.h — Notepad popout panel (docs/22-NOTEPAD-PLAN.md, NT3).
 *
 * A bar-adjacent popout, structural clone of clip_picker.c (docs/13-POPOUTS-
 * SPEC.md sec.4 conventions: layer OVERLAY, ALIGN_END, exclusive keyboard),
 * showing a tabbed multi-line text editor (src/ui/text_edit.h) backed by
 * per-tab plain-text files (src/services/notepad_storage.h). Header (title +
 * close), a tab strip (switch/close/new), the embedded editor, and a footer
 * status line ("Saved · N chars" / "Unsaved changes").
 *
 * Autosave: dc_notepad_tick() (called ~1Hz from main.c's loop, NT4) flushes
 * the current tab to disk once it's been dirty for >= 2s with no further
 * edit; dc_notepad_flush() forces an immediate write (Ctrl+S, tab switch,
 * tab close, panel close, app quit).
 *
 * NOT wired into main.c yet (that's NT4) — this file is the panel component
 * only.
 */
#ifndef DC_UI_NOTEPAD_H
#define DC_UI_NOTEPAD_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct wl_surface;

typedef struct dc_notepad dc_notepad;

dc_notepad *dc_notepad_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render);
void dc_notepad_destroy(dc_notepad *np); /* flushes the current tab first */

void dc_notepad_toggle(dc_notepad *np, struct dc_output *output);
void dc_notepad_hide(dc_notepad *np); /* flushes the current tab */
bool dc_notepad_visible(dc_notepad *np);
struct wl_surface *dc_notepad_surface(dc_notepad *np);

/* Escape closes; Ctrl+S flushes; Ctrl+N new tab; Ctrl+W closes the current
 * tab; Ctrl+Tab / Ctrl+Shift+Tab cycle tabs. Anything else is forwarded to
 * dc_text_edit_key(), with ctrl/shift read from dc_wayland_ctrl_down()/
 * dc_wayland_shift_down(). */
void dc_notepad_handle_key(dc_notepad *np, uint32_t keysym, const char *utf8);

/* Click routes to the header close button, a tab chip's body/close-dot, the
 * trailing "+" (new tab) chip, or the editor (click-to-place-cursor). */
void dc_notepad_handle_click(dc_notepad *np, double x, double y);

/* Pointer motion at logical (x, y): hover tracking over the close button and
 * tab chips, and cursor-shape updates. */
void dc_notepad_handle_motion(dc_notepad *np, double x, double y);

/* Pointer left the panel: clears hover. */
void dc_notepad_handle_leave(dc_notepad *np);

/* Wheel scroll: forwarded to the editor when the pointer is over it. */
void dc_notepad_handle_scroll(dc_notepad *np, int steps_v);

/* Autosave debounce check (docs/22 sec.3/NT3): call ~1Hz regardless of
 * visibility. No-op unless the current tab is dirty and has been idle
 * (no further edit) for >= 2s. */
void dc_notepad_tick(dc_notepad *np);

/* Force-write the current tab now (used by Ctrl+S, tab switch/close, panel
 * close, and destroy()). No-op if there's no storage (e.g. XDG dirs
 * unavailable). */
void dc_notepad_flush(dc_notepad *np);

#endif /* DC_UI_NOTEPAD_H */
