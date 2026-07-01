/* bluez.h — Bluetooth adapter/device state via BlueZ on the system bus.
 *
 * Reads whether an adapter is powered and whether any device is connected,
 * cached for a few seconds so it can be polled from the render path cheaply.
 */
#ifndef DC_SERVICES_BLUEZ_H
#define DC_SERVICES_BLUEZ_H

#include <stdbool.h>

struct dc_dbus;

typedef struct dc_bluez_info {
    bool available; /* BlueZ answered */
    bool powered;   /* an adapter is powered on */
    bool connected; /* at least one device is connected */
} dc_bluez_info;

/* Bind the system bus (from dc_dbus). Call once at startup. */
void dc_bluez_init(struct dc_dbus *dbus);

/* Read cached BlueZ state (refreshed at most every few seconds). Returns true
 * if BlueZ is available. */
bool dc_bluez_read(dc_bluez_info *out);

#endif /* DC_SERVICES_BLUEZ_H */
