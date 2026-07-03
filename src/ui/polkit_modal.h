/* polkit_modal.h — polkit authentication prompt (password dialog).
 *
 * A centered, keyboard-interactive wlr-layer-shell overlay, same shape as
 * ui/powermenu.c (dim scrim + centered card, ANCHOR all four edges,
 * KEYBOARD_INTERACTIVITY_EXCLUSIVE) with a masked password field drawn the
 * same way as ui/lock.c's (rounded pill + dot-per-character). Reference:
 * DankMaterialShell's Modals/PolkitAuthModal.qml + PolkitAuthContent.qml
 * (title "Authentication Required", the action's message, the identity being
 * authenticated as, a password field, Cancel/Authenticate buttons, and an
 * inline error line on a wrong password). Driven by services/polkit.c, which
 * owns the actual D-Bus/PAM-helper plumbing — this module only renders and
 * reports user intent (submit password / cancel) back to the caller via
 * callbacks, exactly like every other panel's dc_<panel>_handle_key/click.
 */
#ifndef DC_UI_POLKIT_MODAL_H
#define DC_UI_POLKIT_MODAL_H

#include <stdbool.h>
#include <stdint.h>

struct dc_wayland;
struct dc_egl;
struct dc_render;
struct dc_output;
struct wl_surface;

typedef struct dc_polkit_modal dc_polkit_modal;

/* password: whatever the user typed before Enter/clicking Authenticate (not
 * cleared by the modal itself -- call dc_polkit_modal_set_busy()/_error() or
 * _hide() once the caller knows the outcome). */
typedef void (*dc_polkit_submit_cb)(const char *password, void *user_data);
/* User pressed Escape or clicked Cancel/the scrim. */
typedef void (*dc_polkit_cancel_cb)(void *user_data);

dc_polkit_modal *dc_polkit_modal_create(struct dc_wayland *wl, struct dc_egl *egl,
                                        struct dc_render *render);
void dc_polkit_modal_destroy(dc_polkit_modal *m);

/* Show the dialog on `output` for one authentication request. `message` is
 * the action's prompt text (e.g. "Authentication is required to run a
 * program as the super user"); `identity` is the display string for who's
 * being authenticated as (e.g. "alice" or "root"). Callbacks fire at most
 * once per shown dialog (submit may fire repeatedly across wrong-password
 * re-prompts; cancel fires once and the dialog should then be hidden by the
 * caller). */
void dc_polkit_modal_show(dc_polkit_modal *m, struct dc_output *output, const char *message,
                          const char *identity, dc_polkit_submit_cb on_submit,
                          dc_polkit_cancel_cb on_cancel, void *user_data);
void dc_polkit_modal_hide(dc_polkit_modal *m);
bool dc_polkit_modal_visible(dc_polkit_modal *m);
struct wl_surface *dc_polkit_modal_surface(dc_polkit_modal *m);

/* Wrong password (or a PAM error message): clear the field, show `text` in
 * the error slot, re-enable input. NULL/"" clears the error line. */
void dc_polkit_modal_set_error(dc_polkit_modal *m, const char *text);
/* While the PAM helper is running: disable input + show a busy label instead
 * of the Cancel/Authenticate row (matches DMS's isLoading state). */
void dc_polkit_modal_set_busy(dc_polkit_modal *m, bool busy);

void dc_polkit_modal_handle_key(dc_polkit_modal *m, uint32_t keysym, const char *utf8);
void dc_polkit_modal_handle_click(dc_polkit_modal *m, double x, double y);
void dc_polkit_modal_handle_motion(dc_polkit_modal *m, double x, double y);

#endif /* DC_UI_POLKIT_MODAL_H */
