/* launcher.h — fuzzy application launcher overlay (spotlight-style).
 *
 * A centered wlr-layer overlay with keyboard focus: a search field over a
 * ranked result list of desktop entries. Type to filter, Up/Down to select,
 * Enter to launch, Esc to dismiss. Matches DMS's Spotlight launcher.
 */
#ifndef DC_UI_LAUNCHER_H
#define DC_UI_LAUNCHER_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct wl_surface;

typedef struct dc_launcher dc_launcher;

dc_launcher *dc_launcher_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render);
void dc_launcher_destroy(dc_launcher *l);

void dc_launcher_toggle(dc_launcher *l, struct dc_output *output);
void dc_launcher_hide(dc_launcher *l);
bool dc_launcher_visible(dc_launcher *l);
struct wl_surface *dc_launcher_surface(dc_launcher *l);

/* Feed a key press (from the Wayland keyboard). Handles text/navigation/launch/
 * dismiss. Ignored when the launcher is hidden. */
void dc_launcher_handle_key(dc_launcher *l, uint32_t keysym, const char *utf8);

/* Click at logical (x, y) on the launcher surface: select+launch a row. */
void dc_launcher_handle_click(dc_launcher *l, double x, double y);

#endif /* DC_UI_LAUNCHER_H */
