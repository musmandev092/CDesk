/* clipboard.h — clipboard history via wlr-data-control.
 *
 * Watches the seat's selection, reads text/plain content into a small history
 * ring, and can copy an entry back. Feeds the clipboard picker (bar clipboard
 * icon), matching DMS's clipboard history. See docs/03-SERVICES.
 */
#ifndef DC_SERVICES_CLIPBOARD_H
#define DC_SERVICES_CLIPBOARD_H

struct dc_wayland;
struct dc_loop;

typedef struct dc_clipboard dc_clipboard;

/* Called when the history changes (new copy). */
typedef void (*dc_clip_changed_cb)(void *user_data);

/* Create the watcher (needs the data-control manager + seat). Returns NULL if
 * the compositor lacks wlr-data-control. */
dc_clipboard *dc_clipboard_create(struct dc_wayland *wl, struct dc_loop *loop);
void dc_clipboard_destroy(dc_clipboard *c);

void dc_clipboard_set_changed_cb(dc_clipboard *c, dc_clip_changed_cb cb, void *user_data);

/* Enumerate history newest-first (pointers owned by the service). */
int dc_clipboard_history(dc_clipboard *c, const char **out, int max);
int dc_clipboard_count(dc_clipboard *c);

/* Copy `text` back to the system clipboard (via wl-copy, detached). */
void dc_clipboard_copy(dc_clipboard *c, const char *text);

#endif /* DC_SERVICES_CLIPBOARD_H */
