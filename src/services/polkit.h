/* polkit.h — polkit authentication agent (docs/03-SERVICES.md sec.10).
 *
 * Registers this process as the session's org.freedesktop.PolicyKit1
 * AuthenticationAgent on the system bus (RegisterAuthenticationAgent, subject
 * = our logind session) and exports the AuthenticationAgent interface
 * (BeginAuthentication/CancelAuthentication) polkitd calls back into.
 *
 * Chose the D-Bus-protocol-direct implementation over linking
 * libpolkit-agent-1: polkit-agent-1/polkit-gobject-1 ARE available on this
 * machine (pkg-config --exists succeeds), but they pull in the full
 * GLib/GObject/GIO stack purely to get a PolkitAgentListener wrapper around
 * the exact same sd-bus calls + PAM-helper spawn this file does directly --
 * dankc has zero GLib dependencies anywhere else and its whole pitch is a
 * small footprint (see AGENTS.md: RSS ~145MB vs DMS's qs ~477MB); adding an
 * entire second object system for one optional service isn't worth it. The
 * PAM conversation itself is delegated to the system's
 * /usr/lib/polkit-1/polkit-agent-helper-1 (per docs/03 sec.10), which is
 * exactly what libpolkit-agent-1's PolkitAgentSession would have shelled out
 * to anyway -- so nothing is "hand-rolled" that polkit doesn't already do
 * for us; only the D-Bus plumbing + PAM-helper stdin/stdout framing are
 * ours, following the same sd-bus vtable pattern as services/notifications.c
 * and services/tray.c.
 */
#ifndef DC_SERVICES_POLKIT_H
#define DC_SERVICES_POLKIT_H

#include <stdbool.h>

struct dc_dbus;
struct dc_loop;
struct dc_wayland;
struct dc_polkit_modal;

typedef struct dc_polkit dc_polkit;

/* Registers on `dbus`'s system bus and exports the AuthenticationAgent
 * object. `loop` is used to watch the PAM helper's stdout pipe; `wl` picks
 * which output the modal opens on (first output — polkit prompts are
 * session-wide, not per-monitor); `modal` is the password dialog it drives.
 * Returns NULL only if there's no system bus at all; a failed
 * RegisterAuthenticationAgent call (e.g. another agent already owns this
 * session) is logged and left non-fatal -- dc_polkit_active() reports which
 * happened. */
dc_polkit *dc_polkit_create(struct dc_dbus *dbus, struct dc_loop *loop, struct dc_wayland *wl,
                            struct dc_polkit_modal *modal);
void dc_polkit_destroy(dc_polkit *pk);

/* True once RegisterAuthenticationAgent succeeded (false if a different
 * agent already holds this session, or there's no system bus). */
bool dc_polkit_active(dc_polkit *pk);

#endif /* DC_SERVICES_POLKIT_H */
