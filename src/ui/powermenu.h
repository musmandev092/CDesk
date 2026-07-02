/* powermenu.h — session power menu modal (Lock / Logout / Suspend / Reboot /
 * Shutdown).
 *
 * A centered, keyboard-interactive wlr-layer-shell overlay with a dim scrim
 * behind it, matching DMS's Modals/PowerMenuModal.qml (vertical list, the
 * default non-grid layout). Destructive actions (everything except Lock)
 * require a second activation to confirm — see powermenu.c's top comment for
 * why this replaces DMS's continuous hold-to-confirm gesture.
 *
 * Opened via `dankc ctl power-menu`, following the same
 * dc_<panel>_toggle/hide/visible/surface convention as every other panel
 * (launcher.c, controlcenter.c, settings.c, ...).
 */
#ifndef DC_UI_POWERMENU_H
#define DC_UI_POWERMENU_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct wl_surface;
struct dc_dbus;
struct dc_lock;

typedef struct dc_powermenu dc_powermenu;

/* `lock` is the existing session-lock object (ui/lock.h) — Lock reuses
 * dc_lock_engage() rather than reimplementing locking. `dbus` supplies the
 * system bus for the org.freedesktop.login1 calls (Suspend/Reboot/PowerOff). */
dc_powermenu *dc_powermenu_create(struct dc_wayland *wl, struct dc_egl *egl, struct dc_render *render,
                                  struct dc_dbus *dbus, struct dc_lock *lock);
void dc_powermenu_destroy(dc_powermenu *pm);

void dc_powermenu_toggle(dc_powermenu *pm, struct dc_output *output);
void dc_powermenu_hide(dc_powermenu *pm);
bool dc_powermenu_visible(dc_powermenu *pm);
struct wl_surface *dc_powermenu_surface(dc_powermenu *pm);

/* Up/Down move selection, Enter activates (arms, then confirms on a second
 * Enter for destructive actions), Escape disarms/closes. */
void dc_powermenu_handle_key(dc_powermenu *pm, uint32_t keysym, const char *utf8);

/* Click at logical (x, y): selects + activates the row under the cursor (or
 * closes if outside the card — DMS's onBackgroundClicked). */
void dc_powermenu_handle_click(dc_powermenu *pm, double x, double y);

/* Hover: moves the selection highlight to the row under the cursor. */
void dc_powermenu_handle_motion(dc_powermenu *pm, double x, double y);

/* Testing only: arm-then-confirm action `index` (0=Lock..4=Shutdown) via the
 * exact same internal activation path two real Enter presses / two real
 * clicks on that row would take — no Wayland input synthesis involved. Used
 * by main.c's DANKC_POWERMENU_FIRE env hook to safely verify each action's
 * DANKC_POWER_DRYRUN log line without driving a shared multi-session
 * compositor's pointer/keyboard (see docs/13-POPOUTS-SPEC.md verification
 * notes: ydotool targets whatever surface currently holds focus, which in a
 * shared session may not be this process's own overlay). */
void dc_powermenu_debug_fire(dc_powermenu *pm, int index);

#endif /* DC_UI_POWERMENU_H */
