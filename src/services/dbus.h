/* dbus.h — shared sd-bus connections (system + user) on the event loop.
 *
 * Foundation for the D-Bus-backed services (BlueZ, UPower, MPRIS, tray, ...).
 * See docs/03-SERVICES.md.
 */
#ifndef DC_SERVICES_DBUS_H
#define DC_SERVICES_DBUS_H

#include <systemd/sd-bus.h>

struct dc_loop;

typedef struct dc_dbus {
    sd_bus *system; /* UPower, logind, BlueZ, NetworkManager, ...; NULL if unavailable */
    sd_bus *user;   /* Notifications, MPRIS, StatusNotifier; NULL if unavailable */
} dc_dbus;

/* Open the system + user buses. Returns NULL only if both fail. */
dc_dbus *dc_dbus_connect(void);
void dc_dbus_destroy(dc_dbus *bus);

/* Drive the buses from the event loop (process on readable, flush on prepare). */
void dc_dbus_integrate(dc_dbus *bus, struct dc_loop *loop);

#endif /* DC_SERVICES_DBUS_H */
