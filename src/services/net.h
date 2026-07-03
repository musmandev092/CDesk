/* net.h — Wi-Fi link state: NetworkManager D-Bus (preferred) or sysfs/nmcli
 * (fallback when NM isn't on the bus).
 *
 * docs/15-PERF-PLAN.md T2.2: the bar's SSID+signal readout used to fork
 * `nmcli dev wifi list` on a timer (~20 forks/min at idle). dc_net_init()
 * subscribes to org.freedesktop.NetworkManager PropertiesChanged on the
 * system bus (services/dbus.c) instead, so the cache updates event-driven,
 * off a signal, with zero forks. If the system bus or NetworkManager itself
 * is unavailable, dc_net_wifi() transparently falls back to the original
 * nmcli-popen path (still forks, but only then).
 */
#ifndef DC_SERVICES_NET_H
#define DC_SERVICES_NET_H

#include <stdbool.h>
#include <stddef.h>

struct dc_dbus;

typedef struct dc_net_info {
    bool has_wifi;   /* a wl* interface exists */
    bool connected;  /* it is operationally up */

    /* Only meaningful when `connected`. Sourced from NetworkManager's D-Bus
     * cache (dc_net_init()) when available; falls back to a briefly-cached
     * `nmcli` popen() otherwise. `ssid` is empty and `signal_percent` is -1
     * if neither source has anything to report. */
    char ssid[64];
    int signal_percent; /* 0-100, or -1 if unknown */
} dc_net_info;

/* Bind the system bus and subscribe to NetworkManager's Wi-Fi device +
 * active-access-point PropertiesChanged signals (event-driven SSID/signal
 * cache -- no forks). Call once at startup, same convention as
 * dc_bluez_init()/dc_power_init(). Safe no-op (dc_net_wifi() falls back to
 * nmcli) if `dbus` is NULL, the system bus is unavailable, or no
 * NetworkManager Wi-Fi device is found. */
void dc_net_init(struct dc_dbus *dbus);

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
 * `known` set -- a secured+unknown SSID needs a password, see
 * dc_net_wifi_connect_psk() below (docs/13-POPOUTS-SPEC.md sec.1). */
void dc_net_wifi_connect(const char *ssid);

/* --- Wi-Fi password entry (control-center inline password field, W1.1) --
 *
 * Unlike dc_net_wifi_connect() above, connecting with a fresh password needs
 * to report back success/failure (wrong password should re-prompt inline
 * instead of silently failing), so this follows the same async fork+pipe+
 * non-blocking-drain shape as the scan list -- one nmcli child at a time,
 * its combined stdout+stderr captured and inspected once it exits (SIGCHLD is
 * SIG_IGN process-wide, so there is no exit status to waitpid() on; nmcli's
 * own "Error: ..." text on failure is the only signal available, same as a
 * user watching a terminal would use). */
typedef enum {
    DC_NET_CONNECT_IDLE = 0,
    DC_NET_CONNECT_IN_PROGRESS,
    DC_NET_CONNECT_SUCCESS,
    DC_NET_CONNECT_FAILED,
} dc_net_connect_state;

/* Start connecting to `ssid` with `psk` (`nmcli dev wifi connect <ssid>
 * password <psk>`), replacing any previous in-flight connect job. Async --
 * call dc_net_wifi_connect_poll() to progress/observe it.
 *
 * If the environment variable DANKC_WIFI_DRYRUN is set (any value), no
 * `nmcli` process is spawned: the exact command line that would have run is
 * logged via dc_info() instead, and the job resolves to
 * DC_NET_CONNECT_SUCCESS on its own after a short simulated delay -- lets the
 * whole password-entry -> connecting -> connected UI flow be exercised and
 * screenshotted without touching the user's real network. */
void dc_net_wifi_connect_psk(const char *ssid, const char *psk);

/* Progress and report the in-flight connect job (non-blocking -- safe to
 * call every render frame while a password prompt/"Connecting..." row is
 * shown, same contract as dc_net_wifi_scan()). On DC_NET_CONNECT_FAILED,
 * `err` (if non-NULL) is filled with a short human-readable reason; left
 * untouched otherwise. A successful connect also nudges the scan list to
 * refresh soon so the row flips to "Connected" without waiting a full
 * DC_NET_SCAN_REFRESH_SEC. */
dc_net_connect_state dc_net_wifi_connect_poll(char *err, size_t err_sz);

/* Abort any in-flight connect job and reset to DC_NET_CONNECT_IDLE (killing
 * the nmcli child if one is running). Called after a terminal state
 * (success/failure) has been consumed, or when the user cancels the
 * password prompt. */
void dc_net_wifi_connect_reset(void);

/* --- Ethernet (wired) -----------------------------------------------------
 * docs/18-WIFI-BT-PLAN.md sec.2.1 -- native NetworkManager D-Bus, no nmcli.
 * dc_net_init() resolves the first NM_DEVICE_TYPE_ETHERNET device (if any)
 * the same way it resolves the Wi-Fi device, and subscribes to its
 * PropertiesChanged (device object) plus its current Ip4Config object's
 * PropertiesChanged (re-subscribed whenever Device.Ip4Config points at a new
 * object, which NM does on every reconfigure) -- event-driven, zero forks. */
typedef enum {
    DC_NET_ETH_UNAVAILABLE = 0, /* device missing/unmanaged, or NM reports UNAVAILABLE with a
                                 * cable present (rfkilled, no driver, etc.) */
    DC_NET_ETH_NO_CABLE,        /* device present but Wired.Carrier == false (unplugged) */
    DC_NET_ETH_DISCONNECTED,    /* cable present, no active connection */
    DC_NET_ETH_CONNECTING,      /* activating (NM_DEVICE_STATE_PREPARE..SECONDARIES) */
    DC_NET_ETH_CONNECTED,       /* NM_DEVICE_STATE_ACTIVATED, IP configured */
} dc_net_eth_state;

typedef struct dc_net_eth_info {
    bool has_device; /* an ethernet Device exists on this system at all */
    dc_net_eth_state state;

    char device_name[32];     /* Device.Interface, e.g. "enp0s31f6" */
    char connection_name[64]; /* active connection's Id; empty if none active */
    char mac[32];              /* Wired.HwAddress (falls back to Device.HwAddress) */
    unsigned link_speed_mbps;  /* Wired.Speed; 0 if link is down/unknown */

    /* IPv4, from the active Device.Ip4Config object. All empty/zero if not
     * connected. Only the first address/gateway is surfaced (multi-address
     * setups are rare enough not to need a full list here). */
    char ipv4_address[64];
    unsigned ipv4_prefix;
    char ipv4_gateway[64];
#define DC_NET_ETH_DNS_MAX 3
    char ipv4_dns[DC_NET_ETH_DNS_MAX][64];
    int ipv4_dns_count;
} dc_net_eth_info;

/* Refresh (from the live D-Bus cache maintained by dc_net_init()'s
 * subscriptions -- no I/O on this call) and copy the current wired-device
 * state into `out`. Returns `out->has_device`. Safe to call every frame. */
bool dc_net_ethernet(dc_net_eth_info *out);

/* Activate the wired device: NetworkManager.ActivateConnection() with the
 * device's first NM-reported AvailableConnections entry if one exists, else
 * "/" (let NM pick/auto-create a profile for the device). Fire-and-forget,
 * same contract as dc_net_wifi_connect(). No-op if there is no ethernet
 * device. Gated by DANKC_NET_DRYRUN (see the header comment above
 * dc_net_hotspot_start() for the shared dry-run convention) -- logs the exact
 * ActivateConnection args instead of calling. */
void dc_net_eth_connect(void);

/* Deactivate the wired device's current active connection
 * (NetworkManager.DeactivateConnection() on Device.ActiveConnection).
 * No-op if the device has no active connection. Same DANKC_NET_DRYRUN gate
 * as dc_net_eth_connect(). */
void dc_net_eth_disconnect(void);

/* --- Wi-Fi known/saved networks --------------------------------------------
 * docs/18-WIFI-BT-PLAN.md sec.2.3 -- org.freedesktop.NetworkManager.Settings,
 * independent of any live scan (a saved network out of range still shows up
 * here). */
#define DC_NET_SAVED_MAX 32

typedef struct dc_net_saved_net {
    char path[64]; /* Settings/Connection object path -- stable id to pass to
                    * dc_net_saved_forget()/dc_net_saved_set_autoconnect() */
    char id[64];   /* connection.id, e.g. "BAIHQ" -- display name */
    char ssid[64]; /* decoded 802-11-wireless.ssid */
    bool autoconnect; /* connection.autoconnect (NM default is true when the
                        * key is absent from GetSettings -- treated as true) */
} dc_net_saved_net;

/* List all saved 802-11-wireless connections (Settings.ListConnections +
 * per-connection GetSettings, filtered to connection.type ==
 * "802-11-wireless"). Synchronous -- a handful of small IPC round trips, no
 * fork, cheap enough to call each time the "Saved Networks" panel is shown
 * (same cost class as dc_net_ethernet(), not the scan list's fork+8s-cache
 * shape). Returns the number of entries written to `out` (capped at `max`).
 * Also refreshes the internal cache used by dc_net_saved_find_by_ssid(). */
int dc_net_saved_list(dc_net_saved_net *out, int max);

/* Delete a saved connection (Settings.Connection.Delete() on `path`, as
 * returned in dc_net_saved_net.path). Returns true if the D-Bus call
 * succeeded (or, under DANKC_NET_DRYRUN, was logged instead of sent). */
bool dc_net_saved_forget(const char *path);

/* Toggle connection.autoconnect on a saved connection (Settings.Connection.
 * Update() with the full settings dict re-read via GetSettings and just the
 * "autoconnect" key rewritten -- Update() replaces the whole connection, it
 * doesn't merge). Returns true on success (or under DANKC_NET_DRYRUN). */
bool dc_net_saved_set_autoconnect(const char *path, bool enable);

/* Look up a saved connection by SSID in the cache populated by the most
 * recent dc_net_saved_list() call (call that first). Returns a pointer into
 * that cache -- valid only until the next dc_net_saved_list() call, do not
 * retain across frames -- or NULL if no saved 802-11-wireless connection
 * matches. Lets the UI answer "is this scanned SSID already saved?" without
 * a second D-Bus round trip. */
const dc_net_saved_net *dc_net_saved_find_by_ssid(const char *ssid);

/* --- Hotspot (Wi-Fi access-point mode) -------------------------------------
 * docs/18-WIFI-BT-PLAN.md sec.2.2 -- NetworkManager.AddAndActivateConnection()
 * on the Wi-Fi device (the same one dc_net_init() resolves for scan/connect)
 * with an ephemeral 802-11-wireless/mode=ap connection; ipv4.method=shared
 * tells NM to run its own DHCP server + NAT, no hostapd/dnsmasq wrangling
 * needed here.
 *
 * IMPORTANT: if the Wi-Fi device is currently connected as a client (see
 * dc_net_wifi()/dc_net_wifi_scan()'s `in_use` AP), starting the hotspot
 * activates the AP-mode connection *on that same radio* and NetworkManager
 * will tear down the client connection to do it -- a single Wi-Fi adapter
 * cannot be a client and an AP at once. This is NOT silently hidden: the
 * caller gets a distinct dc_net_hotspot_start() return value
 * (DC_NET_HOTSPOT_START_WAS_CONNECTED) in that case so the (future) UI can
 * warn the user before/while it happens, and it's logged via dc_warn(). */
typedef enum {
    DC_NET_HOTSPOT_START_OK = 0,          /* started; Wi-Fi device was idle/disconnected */
    DC_NET_HOTSPOT_START_WAS_CONNECTED,   /* started, but a client connection was dropped to do it */
    DC_NET_HOTSPOT_START_NO_DEVICE = -1,  /* no Wi-Fi device found */
    DC_NET_HOTSPOT_START_DBUS_FAILED = -2, /* AddAndActivateConnection itself failed/rejected */
} dc_net_hotspot_start_result;

/* Start an AP-mode connection named after `ssid` on the Wi-Fi device.
 * `password` may be NULL/empty for an open hotspot (omits the
 * 802-11-wireless-security group entirely); otherwise WPA-PSK is used.
 * `band` may be NULL ("bg" or "a" otherwise) to let NM pick. See the dry-run
 * note above dc_net_hotspot_stop(). */
dc_net_hotspot_start_result dc_net_hotspot_start(const char *ssid, const char *password,
                                                 const char *band);

/* Deactivate and delete the ephemeral hotspot connection (whichever one is
 * currently active in AP mode on the Wi-Fi device -- not just one dankc
 * itself started this run, so this also cleans up a hotspot left running
 * from a previous session). No-op if none is active.
 *
 * If the environment variable DANKC_NET_DRYRUN is set (any value), no D-Bus
 * write call is made for dc_net_hotspot_start()/dc_net_hotspot_stop() (nor
 * for the saved-network writes below, nor dc_net_eth_connect()/
 * dc_net_eth_disconnect()): the exact destination/path/interface/method and
 * full argument set (including the AddAndActivateConnection settings dict)
 * are logged via dc_info() instead, mirroring DANKC_WIFI_DRYRUN's existing
 * convention for dc_net_wifi_connect_psk(). Both env vars can be set at once
 * without conflict. */
void dc_net_hotspot_stop(void);

/* True if the Wi-Fi device currently has an AP-mode connection active
 * (Device.Wireless.Mode == NM_802_11_MODE_AP); on true, `ssid_out` (if
 * non-NULL) is filled with the active connection's SSID. */
bool dc_net_hotspot_active(char *ssid_out, size_t len);

#endif /* DC_SERVICES_NET_H */
