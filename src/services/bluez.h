/* bluez.h — Bluetooth adapter/device state via BlueZ on the system bus.
 *
 * Reads whether an adapter is powered and whether any device is connected,
 * cached for a few seconds so it can be polled from the render path cheaply.
 */
#ifndef DC_SERVICES_BLUEZ_H
#define DC_SERVICES_BLUEZ_H

#include <stdbool.h>

struct dc_dbus;

/* One paired/nearby device, from org.bluez.Device1's GetManagedObjects
 * properties (docs/13-POPOUTS-SPEC.md sec.1 bluetooth section). `mac` is the
 * colon-separated address recovered from the object path
 * (/org/bluez/hci0/dev_AA_BB_CC_DD_EE_FF), suitable for `bluetoothctl
 * connect/disconnect <mac>`. */
#define DC_BLUEZ_MAX_DEVICES 16

typedef struct dc_bluez_device {
    char mac[18];
    char name[64];
    bool paired;
    bool connected;
} dc_bluez_device;

typedef struct dc_bluez_info {
    bool available; /* BlueZ answered */
    bool powered;   /* an adapter is powered on */
    bool connected; /* at least one device is connected */

    dc_bluez_device devices[DC_BLUEZ_MAX_DEVICES];
    int device_count; /* paired-or-connected devices, nearest first (path order) */
} dc_bluez_info;

/* Bind the system bus (from dc_dbus). Call once at startup. */
void dc_bluez_init(struct dc_dbus *dbus);

/* Read cached BlueZ state (refreshed at most every few seconds). Returns true
 * if BlueZ is available. */
bool dc_bluez_read(dc_bluez_info *out);

/* Connect/disconnect a device by MAC, fire-and-forget (`bluetoothctl connect|
 * disconnect <mac>`, detached -- same run-detached shape as
 * services/audio.c's dc_audio_set_volume()). */
void dc_bluez_connect(const char *mac);
void dc_bluez_disconnect(const char *mac);

#endif /* DC_SERVICES_BLUEZ_H */
