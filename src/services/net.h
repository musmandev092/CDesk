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

/* --- Wi-Fi scan list (control-center expandable network section) --------
 *
 * `nmcli dev wifi list` can block for well over a second (it triggers a
 * fresh scan), so this follows services/weather.c's async fork+pipe+
 * non-blocking-read pattern instead of net.c's own dc_net_wifi()'s plain
 * popen() (that call only ever reads one already-buffered line from a
 * `--rescan no` query, cheap enough to stay synchronous). */
#define DC_NET_SCAN_MAX 12

typedef struct dc_net_wifi_ap {
    char ssid[64];
    int signal_percent; /* 0-100 */
    bool secured;        /* SECURITY column non-empty */
    bool in_use;         /* the currently-connected AP */
    bool known;          /* a saved NetworkManager connection matches this SSID */
} dc_net_wifi_ap;

/* Copy up to `max` cached scan results into `out` (freshest completed scan;
 * empty until the first one finishes). Kicks off a new async scan if none is
 * in flight and the refresh interval has elapsed, and drains an in-flight
 * scan's pipe (non-blocking) -- safe to call every render frame while the
 * network section is expanded, same contract as dc_weather_get(). Returns
 * the number of APs written to `out`. */
int dc_net_wifi_scan(dc_net_wifi_ap *out, int max);

/* Connect to `ssid`, fire-and-forget (`nmcli dev wifi connect`, detached --
 * relies on NetworkManager reusing a saved connection's credentials for
 * already-known SSIDs). Only meaningful for open networks or ones with
 * `known` set; the control center shows a "needs password" hint instead of
 * calling this for a secured+unknown SSID (no inline password entry yet --
 * see docs/13-POPOUTS-SPEC.md sec.1 TODO). */
void dc_net_wifi_connect(const char *ssid);

#endif /* DC_SERVICES_NET_H */
