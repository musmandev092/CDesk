/* keybinds_modal.h — the keybind cheat-sheet overlay.
 *
 * A centered, keyboard-interactive wlr-layer-shell overlay (same surface/scrim/
 * animation pattern as powermenu.c) that lists the user's niri keybinds in
 * categorized, multi-column masonry — matching DankMaterialShell's
 * Modals/KeybindsModal.qml + KeybindsContent.qml *read-only* cheat-sheet view
 * (this is not DMS's editable keybind-remapping UI, just the "what are my
 * shortcuts" overlay).
 *
 * Source of truth: ~/.config/niri/config.kdl (and any `include "..."` files it
 * references, e.g. DMS's own dms/binds.kdl) is parsed tolerantly at open time
 * -- see keybinds_modal.c's kb_parse_config() for the exact grammar subset
 * understood. This is deliberately distinct from the existing `dankc keybinds`
 * CLI (main.c's print_keybinds()), which only prints a KDL snippet for
 * dankc's own control commands to paste into a binds{} block; this overlay
 * reads the user's *actual* live binds back out and displays them.
 *
 * Opened via `dankc ctl keybinds-overlay`, following the same
 * dc_<panel>_toggle/hide/visible/surface convention as every other panel.
 */
#ifndef DC_UI_KEYBINDS_MODAL_H
#define DC_UI_KEYBINDS_MODAL_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct wl_surface;

typedef struct dc_keybinds_modal dc_keybinds_modal;

dc_keybinds_modal *dc_keybinds_modal_create(struct dc_wayland *wl, struct dc_egl *egl,
                                            struct dc_render *render);
void dc_keybinds_modal_destroy(dc_keybinds_modal *kb);

void dc_keybinds_modal_toggle(dc_keybinds_modal *kb, struct dc_output *output);
void dc_keybinds_modal_hide(dc_keybinds_modal *kb);
bool dc_keybinds_modal_visible(dc_keybinds_modal *kb);
struct wl_surface *dc_keybinds_modal_surface(dc_keybinds_modal *kb);

/* Escape closes; all other keys are ignored (read-only overlay, no search
 * field/editing -- see the file header). */
void dc_keybinds_modal_handle_key(dc_keybinds_modal *kb, uint32_t keysym, const char *utf8);

/* Click outside the card dismisses (DMS's onBackgroundClicked); clicks inside
 * the card are a no-op (nothing is clickable in the read-only list itself). */
void dc_keybinds_modal_handle_click(dc_keybinds_modal *kb, double x, double y);

/* Mouse-wheel scroll of the masonry list when it overflows the card. */
void dc_keybinds_modal_handle_scroll(dc_keybinds_modal *kb, int steps_v);

#endif /* DC_UI_KEYBINDS_MODAL_H */
