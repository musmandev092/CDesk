/* net.h — Wi-Fi link state from sysfs (no NetworkManager dependency yet).
 *
 * A light stand-in until the NetworkManager sd-bus service lands (M3).
 */
#ifndef DC_SERVICES_NET_H
#define DC_SERVICES_NET_H

#include <stdbool.h>

typedef struct dc_net_info {
    bool has_wifi;   /* a wl* interface exists */
    bool connected;  /* it is operationally up */

    /* Only meaningful when `connected`; from `nmcli` (already present on any
     * NetworkManager-managed system -- no new IPC mechanism introduced).
     * `ssid` is empty and `signal_percent` is -1 if nmcli has nothing to
     * report (not installed, or a non-NM Wi-Fi setup). */
    char ssid[64];
    int signal_percent; /* 0-100, or -1 if unknown */
} dc_net_info;

/* Fill `out` from /sys/class/net. Returns true if a Wi-Fi interface exists. */
bool dc_net_wifi(dc_net_info *out);

#endif /* DC_SERVICES_NET_H */
