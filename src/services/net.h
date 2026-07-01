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
} dc_net_info;

/* Fill `out` from /sys/class/net. Returns true if a Wi-Fi interface exists. */
bool dc_net_wifi(dc_net_info *out);

#endif /* DC_SERVICES_NET_H */
