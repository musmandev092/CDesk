/* clip_picker.h — clipboard history picker overlay.
 *
 * A bar-adjacent popout (docs/13-POPOUTS-SPEC.md sec.4) listing recent
 * clipboard entries (text and images) with search, pin, and delete; type to
 * filter, Up/Down to select, Enter to copy it back, Esc to dismiss. Opened
 * from the bar clipboard icon. Matches DMS's ClipboardHistoryPopout.
 */
#ifndef DC_UI_CLIP_PICKER_H
#define DC_UI_CLIP_PICKER_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct dc_clipboard;
struct wl_surface;

typedef struct dc_clip_picker dc_clip_picker;

dc_clip_picker *dc_clip_picker_create(struct dc_wayland *wl, struct dc_egl *egl,
                                      struct dc_render *render, struct dc_clipboard *clipboard);
void dc_clip_picker_destroy(dc_clip_picker *p);

void dc_clip_picker_toggle(dc_clip_picker *p, struct dc_output *output);
void dc_clip_picker_hide(dc_clip_picker *p);
bool dc_clip_picker_visible(dc_clip_picker *p);
struct wl_surface *dc_clip_picker_surface(dc_clip_picker *p);

void dc_clip_picker_handle_key(dc_clip_picker *p, uint32_t keysym, const char *utf8);
void dc_clip_picker_handle_click(dc_clip_picker *p, double x, double y);
void dc_clip_picker_handle_scroll(dc_clip_picker *p, int steps_v);

/* Pointer motion at logical (x, y): hover tracking (rows, pin/delete, close,
 * clear); hovering a row body also moves the keyboard selection onto it. */
void dc_clip_picker_handle_motion(dc_clip_picker *p, double x, double y);

/* Pointer left the panel: clears hover. */
void dc_clip_picker_handle_leave(dc_clip_picker *p);

#endif /* DC_UI_CLIP_PICKER_H */
