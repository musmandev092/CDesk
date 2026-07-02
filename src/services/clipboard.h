/* clipboard.h — clipboard history via wlr-data-control.
 *
 * Watches the seat's selection and reads text/plain *and* image/{png,jpeg}
 * content (up to a size cap) into a small history list; can copy an entry
 * back, delete it, or pin it. Pinned *text* entries are persisted to
 * ~/.local/state/dankc/clipboard_pins.json (cJSON) and reloaded on the next
 * run; pinned *images* are not persisted (session-only -- re-pin after a
 * restart if still on the clipboard/history).
 * Feeds the clipboard picker (bar clipboard icon), matching DMS's clipboard
 * history (docs/13-POPOUTS-SPEC.md sec.4). See docs/03-SERVICES.
 */
#ifndef DC_SERVICES_CLIPBOARD_H
#define DC_SERVICES_CLIPBOARD_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct dc_wayland;
struct dc_loop;

typedef struct dc_clipboard dc_clipboard;

typedef enum {
    DC_CLIP_TEXT,
    DC_CLIP_IMAGE,
} dc_clip_kind;

/* One history entry, as exposed to callers (ui/clip_picker.c). Pointers are
 * owned by the service and stay valid until the next call that mutates the
 * history (copy/delete/pin/clear-all) or dc_clipboard_list() itself. */
typedef struct {
    uint64_t id; /* stable, monotonically increasing -- survives reordering */
    dc_clip_kind kind;
    bool pinned;

    const char *text; /* DC_CLIP_TEXT only: NUL-terminated */
    size_t text_len;

    const unsigned char *image_data; /* DC_CLIP_IMAGE only: encoded bytes */
    size_t image_len;
    int width, height;    /* decoded from the image header; 0 if unknown */
    const char *image_ext; /* "png" or "jpg", DC_CLIP_IMAGE only */
} dc_clip_entry;

/* Called when the history changes (new copy, delete, pin toggle, clear). */
typedef void (*dc_clip_changed_cb)(void *user_data);

/* Create the watcher (needs the data-control manager + seat). Returns NULL if
 * the compositor lacks wlr-data-control. */
dc_clipboard *dc_clipboard_create(struct dc_wayland *wl, struct dc_loop *loop);
void dc_clipboard_destroy(dc_clipboard *c);

void dc_clipboard_set_changed_cb(dc_clipboard *c, dc_clip_changed_cb cb, void *user_data);

/* Enumerate history: pinned entries first (newest-pinned first), then
 * unpinned newest-first -- matches DMS's "saved sort to top" behavior. */
int dc_clipboard_list(dc_clipboard *c, dc_clip_entry *out, int max);
int dc_clipboard_count(dc_clipboard *c);

/* Copy an entry back to the system clipboard (via wl-copy, detached). */
void dc_clipboard_copy(dc_clipboard *c, uint64_t id);
void dc_clipboard_delete(dc_clipboard *c, uint64_t id);
void dc_clipboard_toggle_pin(dc_clipboard *c, uint64_t id);
/* Clear all *unpinned* entries -- matches DMS's ClipboardService.clearAll(),
 * which keeps pinned/"saved" entries. */
void dc_clipboard_clear_all(dc_clipboard *c);

#endif /* DC_SERVICES_CLIPBOARD_H */
