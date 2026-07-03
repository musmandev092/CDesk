#include "services/bluez.h"

#include "core/log.h"
#include "dc.h"
#include "services/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DC_BLUEZ_REFRESH_SECS 3

/* MAC used by the DANKC_BT_FAKE_DEVICE debug hook (W3.1 verification) --
 * fake, never a real device's address (BlueZ addresses starting AA:BB:CC
 * aren't allocated to any real vendor OUI). */
#define DC_BLUEZ_FAKE_MAC "AA:BB:CC:DD:EE:FF"

static sd_bus *g_system = NULL;
static dc_bluez_info g_cache;
static time_t g_last_refresh = 0;

/* Adapter object path (e.g. "/org/bluez/hci0"), captured the first time
 * scan_objects() sees an Adapter1 interface -- StartDiscovery/StopDiscovery
 * and pairing both need it. */
static char g_adapter_path[64] = {0};
static bool g_discovering = false;

static void bluez_register_agent(void);
static void bluez_resolve_fake_pair(bool ok);

void dc_bluez_init(struct dc_dbus *dbus)
{
    g_system = dbus ? dbus->system : NULL;
    if (g_system)
        bluez_register_agent();
}

/* Recover "AA:BB:CC:DD:EE:FF" from a device object path
 * (".../dev_AA_BB_CC_DD_EE_FF"). Writes "" if the path doesn't match. */
static void mac_from_path(const char *path, char *out, size_t out_sz)
{
    out[0] = '\0';
    if (!path)
        return;
    const char *dev = strstr(path, "dev_");
    if (!dev)
        return;
    dev += 4;
    size_t j = 0;
    for (; dev[j] && j < out_sz - 1; j++)
        out[j] = dev[j] == '_' ? ':' : dev[j];
    out[j] = '\0';
}

/* Scan GetManagedObjects for a powered adapter, any connected device, and the
 * full paired/nearby device list (docs/13-POPOUTS-SPEC.md sec.1 bluetooth
 * expandable section). */
static void scan_objects(dc_bluez_info *info)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(g_system, "org.bluez", "/",
                               "org.freedesktop.DBus.ObjectManager", "GetManagedObjects", &err,
                               &reply, "");
    if (r < 0) {
        sd_bus_error_free(&err);
        return;
    }
    info->available = true;

    if (sd_bus_message_enter_container(reply, 'a', "{oa{sa{sv}}}") < 0)
        goto done;

    while (sd_bus_message_enter_container(reply, 'e', "oa{sa{sv}}") > 0) {
        const char *path = NULL;
        sd_bus_message_read_basic(reply, 'o', &path);

        /* Accumulated across every interface block for this one object, then
         * folded into info->devices[] once the object is fully read (a
         * device's Name/Alias/Paired/Connected properties are all on the
         * single org.bluez.Device1 interface, but reading them incrementally
         * as they stream by is simplest). org.bluez.Battery1 is a *separate*
         * interface BlueZ auto-adds to the same object path once it can
         * decode battery level from the connected profile (HID/HFP/some LE),
         * so its Percentage is folded in here too rather than requiring a
         * second GetManagedObjects pass. */
        bool is_device_obj = false;
        bool dev_paired = false, dev_connected = false, dev_trusted = false;
        bool dev_has_battery = false;
        int dev_battery = -1;
        char dev_name[64] = {0};
        char dev_alias[64] = {0};
        char dev_icon[32] = {0};
        uint32_t dev_class = 0;
        uint16_t dev_appearance = 0;

        if (sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") < 0) {
            sd_bus_message_exit_container(reply);
            break;
        }
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *iface = NULL;
            sd_bus_message_read_basic(reply, 's', &iface);

            bool is_device = iface && strcmp(iface, "org.bluez.Device1") == 0;
            bool is_adapter = iface && strcmp(iface, "org.bluez.Adapter1") == 0;
            bool is_battery = iface && strcmp(iface, "org.bluez.Battery1") == 0;
            if (is_device)
                is_device_obj = true;
            if (is_adapter && path)
                snprintf(g_adapter_path, sizeof(g_adapter_path), "%s", path);

            if (is_device || is_adapter || is_battery) {
                sd_bus_message_enter_container(reply, 'a', "{sv}");
                while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                    const char *prop = NULL;
                    sd_bus_message_read_basic(reply, 's', &prop);
                    if (is_device && prop && strcmp(prop, "Connected") == 0) {
                        int connected = 0;
                        sd_bus_message_enter_container(reply, 'v', "b");
                        sd_bus_message_read_basic(reply, 'b', &connected);
                        sd_bus_message_exit_container(reply);
                        if (connected) {
                            info->connected = true;
                            dev_connected = true;
                        }
                    } else if (is_device && prop && strcmp(prop, "Paired") == 0) {
                        int paired = 0;
                        sd_bus_message_enter_container(reply, 'v', "b");
                        sd_bus_message_read_basic(reply, 'b', &paired);
                        sd_bus_message_exit_container(reply);
                        dev_paired = paired != 0;
                    } else if (is_device && prop && strcmp(prop, "Trusted") == 0) {
                        int trusted = 0;
                        sd_bus_message_enter_container(reply, 'v', "b");
                        sd_bus_message_read_basic(reply, 'b', &trusted);
                        sd_bus_message_exit_container(reply);
                        dev_trusted = trusted != 0;
                    } else if (is_device && prop && strcmp(prop, "Name") == 0) {
                        const char *val = NULL;
                        sd_bus_message_enter_container(reply, 'v', "s");
                        sd_bus_message_read_basic(reply, 's', &val);
                        sd_bus_message_exit_container(reply);
                        if (val)
                            snprintf(dev_name, sizeof(dev_name), "%s", val);
                    } else if (is_device && prop && strcmp(prop, "Alias") == 0) {
                        const char *val = NULL;
                        sd_bus_message_enter_container(reply, 'v', "s");
                        sd_bus_message_read_basic(reply, 's', &val);
                        sd_bus_message_exit_container(reply);
                        if (val)
                            snprintf(dev_alias, sizeof(dev_alias), "%s", val);
                    } else if (is_device && prop && strcmp(prop, "Icon") == 0) {
                        const char *val = NULL;
                        sd_bus_message_enter_container(reply, 'v', "s");
                        sd_bus_message_read_basic(reply, 's', &val);
                        sd_bus_message_exit_container(reply);
                        if (val)
                            snprintf(dev_icon, sizeof(dev_icon), "%s", val);
                    } else if (is_device && prop && strcmp(prop, "Class") == 0) {
                        uint32_t val = 0;
                        sd_bus_message_enter_container(reply, 'v', "u");
                        sd_bus_message_read_basic(reply, 'u', &val);
                        sd_bus_message_exit_container(reply);
                        dev_class = val;
                    } else if (is_device && prop && strcmp(prop, "Appearance") == 0) {
                        uint16_t val = 0;
                        sd_bus_message_enter_container(reply, 'v', "q");
                        sd_bus_message_read_basic(reply, 'q', &val);
                        sd_bus_message_exit_container(reply);
                        dev_appearance = val;
                    } else if (is_battery && prop && strcmp(prop, "Percentage") == 0) {
                        uint8_t val = 0;
                        sd_bus_message_enter_container(reply, 'v', "y");
                        sd_bus_message_read_basic(reply, 'y', &val);
                        sd_bus_message_exit_container(reply);
                        dev_has_battery = true;
                        dev_battery = val;
                    } else if (is_adapter && prop && strcmp(prop, "Powered") == 0) {
                        int powered = 0;
                        sd_bus_message_enter_container(reply, 'v', "b");
                        sd_bus_message_read_basic(reply, 'b', &powered);
                        sd_bus_message_exit_container(reply);
                        if (powered)
                            info->powered = true;
                    } else if (is_adapter && prop && strcmp(prop, "Discoverable") == 0) {
                        int discoverable = 0;
                        sd_bus_message_enter_container(reply, 'v', "b");
                        sd_bus_message_read_basic(reply, 'b', &discoverable);
                        sd_bus_message_exit_container(reply);
                        if (discoverable)
                            info->discoverable = true;
                    } else if (is_adapter && prop && strcmp(prop, "Pairable") == 0) {
                        int pairable = 0;
                        sd_bus_message_enter_container(reply, 'v', "b");
                        sd_bus_message_read_basic(reply, 'b', &pairable);
                        sd_bus_message_exit_container(reply);
                        if (pairable)
                            info->pairable = true;
                    } else {
                        sd_bus_message_skip(reply, "v");
                    }
                    sd_bus_message_exit_container(reply); /* sv */
                }
                sd_bus_message_exit_container(reply); /* a{sv} */
            } else {
                sd_bus_message_skip(reply, "a{sv}");
            }
            sd_bus_message_exit_container(reply); /* sa{sv} */
        }
        sd_bus_message_exit_container(reply); /* a{sa{sv}} */
        sd_bus_message_exit_container(reply); /* oa{sa{sv}} */

        /* Paired (the "known devices" list) or currently connected devices
         * always; unpaired nearby-scan devices too, but only while a
         * discovery is active (W3.1 "Discover" affordance) -- BlueZ only
         * populates/keeps those objects fresh while scanning anyway. */
        if (is_device_obj && (dev_paired || dev_connected || g_discovering) &&
            info->device_count < DC_BLUEZ_MAX_DEVICES) {
            dc_bluez_device *d = &info->devices[info->device_count];
            memset(d, 0, sizeof(*d));
            mac_from_path(path, d->mac, sizeof(d->mac));
            const char *name = dev_name[0] ? dev_name : (dev_alias[0] ? dev_alias : d->mac);
            snprintf(d->name, sizeof(d->name), "%s", name);
            d->paired = dev_paired;
            d->connected = dev_connected;
            d->trusted = dev_trusted;
            d->battery_percent = dev_has_battery ? dev_battery : -1;
            snprintf(d->icon, sizeof(d->icon), "%s", dev_icon);
            d->device_class = dev_class;
            d->appearance = dev_appearance;
            info->device_count++;
        }
    }

done:
    sd_bus_message_unref(reply);
}

/* --- DANKC_BTSVC_TEST verification hook (Wave 1 service-layer testing) -----
 *
 * Env-gated (any value): logs the full device list (name/mac/connected/
 * paired/trusted/battery/icon/class/appearance) plus adapter state once at
 * startup and again every time dc_bluez_read()'s result changes, so this can
 * be checked against `bluetoothctl devices`/`busctl introspect` reality
 * without any UI. No effect on behaviour when unset. */
static dc_bluez_info g_test_last_logged;
static bool g_test_logged_once = false;

static void bluez_test_log(const dc_bluez_info *info)
{
    dc_info("bluez: [DANKC_BTSVC_TEST] adapter available=%d powered=%d discoverable=%d "
            "pairable=%d connected=%d devices=%d",
            info->available, info->powered, info->discoverable, info->pairable, info->connected,
            info->device_count);
    for (int i = 0; i < info->device_count; i++) {
        const dc_bluez_device *d = &info->devices[i];
        char battery_buf[16];
        if (d->battery_percent >= 0)
            snprintf(battery_buf, sizeof(battery_buf), "%d%%", d->battery_percent);
        else
            snprintf(battery_buf, sizeof(battery_buf), "n/a");
        dc_info("bluez: [DANKC_BTSVC_TEST]   %-17s %-24s connected=%d paired=%d trusted=%d "
                "battery=%-4s icon=%-16s class=0x%06x appearance=0x%04x",
                d->mac, d->name, d->connected, d->paired, d->trusted, battery_buf,
                d->icon[0] ? d->icon : "-", (unsigned)d->device_class, (unsigned)d->appearance);
    }
}

static void bluez_test_log_if_changed(const dc_bluez_info *info)
{
    if (!getenv("DANKC_BTSVC_TEST"))
        return;
    /* Byte-for-byte compare against the last-logged snapshot -- both sides
     * are always fully zero-initialised before being filled (scan_objects()
     * memsets g_cache, memsets each device slot), so padding is deterministic
     * and this is a reliable (if slightly conservative) change check for a
     * debug-only hook. */
    if (g_test_logged_once && memcmp(info, &g_test_last_logged, sizeof(*info)) == 0)
        return;
    bluez_test_log(info);
    g_test_last_logged = *info;
    g_test_logged_once = true;
}

bool dc_bluez_read(dc_bluez_info *out)
{
    time_t now = time(NULL);
    if (g_last_refresh != 0 && now - g_last_refresh < DC_BLUEZ_REFRESH_SECS) {
        *out = g_cache;
    } else {
        g_last_refresh = now;
        memset(&g_cache, 0, sizeof(g_cache));
        if (g_system)
            scan_objects(&g_cache);
        *out = g_cache;
    }

    /* DANKC_BT_FAKE_DEVICE=<name> (debug-only, env-gated -- same convention
     * as services/net.c's DANKC_WIFI_FAKE_AP): while a (possibly simulated,
     * see dc_bluez_start_discovery()) discovery is active, append one
     * synthetic unpaired device so the "nearby, tap to pair" row + pairing
     * flow can be screenshotted on a machine with no real Bluetooth hardware
     * nearby. No-op when unset or not discovering. */
    const char *fake = getenv("DANKC_BT_FAKE_DEVICE");
    if (fake && fake[0] && g_discovering && out->device_count < DC_BLUEZ_MAX_DEVICES) {
        dc_bluez_device *d = &out->devices[out->device_count];
        memset(d, 0, sizeof(*d));
        snprintf(d->mac, sizeof(d->mac), "%s", DC_BLUEZ_FAKE_MAC);
        snprintf(d->name, sizeof(d->name), "%s", fake);
        d->paired = false;
        d->connected = false;
        d->battery_percent = -1; /* no Battery1 interface on the synthetic device */
        out->device_count++;
        out->available = true;
    }

    bluez_test_log_if_changed(out);
    return out->available;
}

int dc_bluez_devices(dc_bluez_device *out, int max)
{
    dc_bluez_info info;
    dc_bluez_read(&info);
    int n = info.device_count < max ? info.device_count : max;
    for (int i = 0; i < n; i++)
        out[i] = info.devices[i];
    return n;
}

/* Fire-and-forget `bluetoothctl <verb> <mac>`, detached (same run-detached
 * shape as services/audio.c's dc_audio_set_volume()) -- bluetoothctl rather
 * than a direct Connect()/Disconnect() D-Bus call since it already handles
 * agent/pairing fallbacks dankc doesn't implement. */
static void run_bluetoothctl(const char *verb, const char *mac)
{
    if (!mac || !mac[0])
        return;
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "bluetoothctl %.16s %.20s", verb, mac);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    /* Force a fresh read on the next dc_bluez_read() instead of serving the
     * pre-click cache for up to DC_BLUEZ_REFRESH_SECS. */
    g_last_refresh = 0;
}

void dc_bluez_connect(const char *mac)
{
    run_bluetoothctl("connect", mac);
}

void dc_bluez_disconnect(const char *mac)
{
    run_bluetoothctl("disconnect", mac);
}

/* Synchronous org.freedesktop.DBus.Properties.Set with a boolean value --
 * shared by the Discoverable/Pairable/Trusted toggles below. A single local
 * system-bus round trip for a settings toggle is fine to do synchronously
 * (same precedent as agent_device_display_name()'s blocking property get and
 * scan_objects()'s blocking GetManagedObjects call, both already in this
 * file); async is reserved for the multi-step pairing chain where blocking
 * would stall the UI for as long as real pairing takes. Returns the sd-bus
 * return code (negative on failure, already logged). */
static int bluez_set_bool_property(const char *path, const char *iface, const char *prop, bool value)
{
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(g_system, &m, "org.bluez", path,
                                           "org.freedesktop.DBus.Properties", "Set");
    if (r < 0) {
        dc_warn("bluez: Properties.Set %s %s.%s: message_new failed: %s", path, iface, prop,
                strerror(-r));
        return r;
    }
    sd_bus_message_append(m, "ss", iface, prop);
    sd_bus_message_open_container(m, 'v', "b");
    sd_bus_message_append(m, "b", (int)value);
    sd_bus_message_close_container(m);

    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call(g_system, m, 0, &err, NULL);
    if (r < 0)
        dc_warn("bluez: Properties.Set %s %s.%s=%s failed: %s", path, iface, prop,
                value ? "true" : "false", err.message ? err.message : strerror(-r));
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    return r;
}

/* --- Adapter settings: Discoverable / Pairable (Wave 1) --------------------
 *
 * Mirrors the existing Powered toggle (which the UI currently drives via
 * `bluetoothctl power on|off`, see ui/settings.c) but as a native
 * Properties.Set, since there's no bluetoothctl-mediated agent/pairing
 * fallback concern for a plain adapter property the way there is for
 * connect/disconnect. */

void dc_bluez_set_discoverable(bool on)
{
    if (!g_adapter_path[0]) {
        dc_warn("bluez: set_discoverable: no adapter known yet");
        return;
    }
    if (getenv("DANKC_BT_DRYRUN")) {
        dc_info("bluez: [DANKC_BT_DRYRUN] would Properties.Set %s org.bluez.Adapter1.Discoverable=%s",
                g_adapter_path, on ? "true" : "false");
        return;
    }
    if (!g_system)
        return;
    if (bluez_set_bool_property(g_adapter_path, "org.bluez.Adapter1", "Discoverable", on) >= 0)
        g_last_refresh = 0;
}

void dc_bluez_set_pairable(bool on)
{
    if (!g_adapter_path[0]) {
        dc_warn("bluez: set_pairable: no adapter known yet");
        return;
    }
    if (getenv("DANKC_BT_DRYRUN")) {
        dc_info("bluez: [DANKC_BT_DRYRUN] would Properties.Set %s org.bluez.Adapter1.Pairable=%s",
                g_adapter_path, on ? "true" : "false");
        return;
    }
    if (!g_system)
        return;
    if (bluez_set_bool_property(g_adapter_path, "org.bluez.Adapter1", "Pairable", on) >= 0)
        g_last_refresh = 0;
}

/* --- Discovery (W3.1) ------------------------------------------------------
 */

void dc_bluez_start_discovery(void)
{
    if (g_discovering)
        return;

    if (!g_system || !g_adapter_path[0]) {
        /* No system bus, or no adapter seen yet -- still let the
         * DANKC_BT_FAKE_DEVICE debug hook work on a machine with no real
         * bluetoothd (docs/14-COMPLETION-PLAN.md W3.1 verification), rather
         * than silently doing nothing. */
        if (getenv("DANKC_BT_FAKE_DEVICE"))
            g_discovering = true;
        return;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_call_method(g_system, "org.bluez", g_adapter_path, "org.bluez.Adapter1",
                               "StartDiscovery", &err, NULL, "");
    if (r < 0) {
        dc_warn("bluez: StartDiscovery failed: %s", err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        if (getenv("DANKC_BT_FAKE_DEVICE"))
            g_discovering = true; /* debug hook still works without real discovery */
        return;
    }
    sd_bus_error_free(&err);
    g_discovering = true;
    g_last_refresh = 0; /* pick up newly-advertised devices on the next read */
}

void dc_bluez_stop_discovery(void)
{
    if (!g_discovering)
        return;
    g_discovering = false;

    if (g_system && g_adapter_path[0]) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        sd_bus_call_method(g_system, "org.bluez", g_adapter_path, "org.bluez.Adapter1",
                           "StopDiscovery", &err, NULL, "");
        sd_bus_error_free(&err);
    }
    g_last_refresh = 0; /* drop unpaired devices from the list promptly */
}

bool dc_bluez_discovering(void)
{
    return g_discovering;
}

/* --- Pairing agent (org.bluez.Agent1, W3.1) --------------------------------
 */

#define DC_BLUEZ_AGENT_PATH "/org/dankc/BluezAgent"

typedef struct {
    dc_bluez_agent_kind kind;
    char device_name[64];
    char passkey_str[8];
    sd_bus_message *pending_msg; /* ref'd; NULL when kind == DC_BLUEZ_AGENT_NONE
                                   * or while simulating (see fake_pending) */
    bool fake_pending; /* DANKC_BT_DRYRUN + DANKC_BT_FAKE_DEVICE simulation --
                        * no real BlueZ call is waiting, dc_bluez_pair_poll()
                        * resolves the pair job once this is answered. */
} bluez_agent_state;

static bluez_agent_state g_agent;

/* Best-effort human-readable name for a Device1 object path -- Alias (falls
 * back to Name-less devices' MAC, same fallback order as scan_objects()'s
 * dev_name/dev_alias/mac chain). Used only for the agent dialog's device
 * label; a failed lookup just shows the MAC instead of a friendly name. */
static void agent_device_display_name(const char *path, char *out, size_t out_sz)
{
    char *alias = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    int r = sd_bus_get_property_string(g_system, "org.bluez", path, "org.bluez.Device1", "Alias",
                                       &err, &alias);
    sd_bus_error_free(&err);
    if (r >= 0 && alias && alias[0]) {
        snprintf(out, out_sz, "%s", alias);
        free(alias);
        return;
    }
    free(alias);
    mac_from_path(path, out, out_sz);
    if (!out[0])
        snprintf(out, out_sz, "device");
}

static int agent_method_release(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    dc_debug("bluez: agent Release()");
    return sd_bus_reply_method_return(m, "");
}

static int agent_method_request_pin_code(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    /* Legacy PIN-code pairing (pre-SSP devices) has no dankc UI yet -- reject
     * rather than silently hanging BlueZ's request. */
    return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected",
                                      "PIN code entry not supported");
}

static int agent_method_display_pin_code(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    const char *device = NULL, *pincode = NULL;
    sd_bus_message_read(m, "os", &device, &pincode);
    dc_info("bluez: DisplayPinCode %s: %s", device ? device : "?", pincode ? pincode : "?");
    return sd_bus_reply_method_return(m, "");
}

static int agent_method_request_passkey(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    if (g_agent.pending_msg || g_agent.fake_pending)
        return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "busy");

    const char *device = NULL;
    sd_bus_message_read(m, "o", &device);

    g_agent.kind = DC_BLUEZ_AGENT_PASSKEY;
    agent_device_display_name(device, g_agent.device_name, sizeof(g_agent.device_name));
    g_agent.passkey_str[0] = '\0';
    g_agent.pending_msg = sd_bus_message_ref(m);
    return 1; /* handled; reply deferred until dc_bluez_agent_respond_passkey() */
}

static int agent_method_display_passkey(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    const char *device = NULL;
    uint32_t passkey = 0;
    uint16_t entered = 0;
    sd_bus_message_read(m, "ouq", &device, &passkey, &entered);
    dc_debug("bluez: DisplayPasskey %s: %06u (%u digits entered)", device ? device : "?",
            (unsigned)passkey, (unsigned)entered);
    return sd_bus_reply_method_return(m, "");
}

static int agent_method_request_confirmation(sd_bus_message *m, void *userdata,
                                             sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    if (g_agent.pending_msg || g_agent.fake_pending)
        return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "busy");

    const char *device = NULL;
    uint32_t passkey = 0;
    sd_bus_message_read(m, "ou", &device, &passkey);

    g_agent.kind = DC_BLUEZ_AGENT_CONFIRM;
    agent_device_display_name(device, g_agent.device_name, sizeof(g_agent.device_name));
    snprintf(g_agent.passkey_str, sizeof(g_agent.passkey_str), "%06u", (unsigned)passkey);
    g_agent.pending_msg = sd_bus_message_ref(m);
    return 1; /* handled; reply deferred until dc_bluez_agent_respond_yesno() */
}

static int agent_method_request_authorization(sd_bus_message *m, void *userdata,
                                              sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    if (g_agent.pending_msg || g_agent.fake_pending)
        return sd_bus_reply_method_errorf(m, "org.bluez.Error.Rejected", "busy");

    const char *device = NULL;
    sd_bus_message_read(m, "o", &device);

    g_agent.kind = DC_BLUEZ_AGENT_AUTHORIZE;
    agent_device_display_name(device, g_agent.device_name, sizeof(g_agent.device_name));
    g_agent.passkey_str[0] = '\0';
    g_agent.pending_msg = sd_bus_message_ref(m);
    return 1; /* handled; reply deferred until dc_bluez_agent_respond_yesno() */
}

static int agent_method_authorize_service(sd_bus_message *m, void *userdata,
                                          sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    const char *device = NULL, *uuid = NULL;
    sd_bus_message_read(m, "os", &device, &uuid);
    /* dankc has no per-service trust UI yet -- auto-allow (same posture as
     * bluetoothctl's default agent), a paired+trusted device using one of
     * its own services is the common case and not worth an extra prompt. */
    dc_debug("bluez: AuthorizeService %s %s (auto-allow)", device ? device : "?",
            uuid ? uuid : "?");
    return sd_bus_reply_method_return(m, "");
}

static int agent_method_cancel(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    dc_info("bluez: agent Cancel() -- BlueZ withdrew the pending request");
    if (g_agent.pending_msg) {
        sd_bus_reply_method_errorf(g_agent.pending_msg, "org.bluez.Error.Canceled", "Cancelled");
        sd_bus_message_unref(g_agent.pending_msg);
        g_agent.pending_msg = NULL;
    }
    g_agent.kind = DC_BLUEZ_AGENT_NONE;
    g_agent.fake_pending = false;
    return sd_bus_reply_method_return(m, "");
}

static const sd_bus_vtable bluez_agent_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Release", "", "", agent_method_release, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPinCode", "o", "s", agent_method_request_pin_code,
                 SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPinCode", "os", "", agent_method_display_pin_code,
                 SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestPasskey", "o", "u", agent_method_request_passkey,
                 SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("DisplayPasskey", "ouq", "", agent_method_display_passkey,
                 SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestConfirmation", "ou", "", agent_method_request_confirmation,
                 SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("RequestAuthorization", "o", "", agent_method_request_authorization,
                 SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("AuthorizeService", "os", "", agent_method_authorize_service,
                 SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("Cancel", "", "", agent_method_cancel, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_VTABLE_END,
};

static sd_bus_slot *g_agent_vtable_slot;

/* Export dankc's Agent1 object and register it with BlueZ's AgentManager1 --
 * same "export vtable, call the manager's Register method, log+continue if
 * it's rejected" shape as services/polkit.c's register_agent(), but BlueZ
 * allows registering an agent per-connection regardless of any other
 * process's agent (Pair() calls made over *this* bus connection use *this*
 * agent), so failure here only means dankc can't answer PIN/passkey/
 * confirmation prompts -- "Just Works" pairing (most modern headphones/
 * speakers) is unaffected either way. */
static void bluez_register_agent(void)
{
    int r = sd_bus_add_object_vtable(g_system, &g_agent_vtable_slot, DC_BLUEZ_AGENT_PATH,
                                     "org.bluez.Agent1", bluez_agent_vtable, NULL);
    if (r < 0) {
        dc_warn("bluez: sd_bus_add_object_vtable(Agent1) failed: %s", strerror(-r));
        return;
    }

    sd_bus_error err = SD_BUS_ERROR_NULL;
    r = sd_bus_call_method(g_system, "org.bluez", "/org/bluez", "org.bluez.AgentManager1",
                           "RegisterAgent", &err, NULL, "os", DC_BLUEZ_AGENT_PATH,
                           "KeyboardDisplay");
    if (r < 0) {
        dc_warn("bluez: RegisterAgent failed (%s) -- another Bluetooth agent is probably already "
                "registered; dankc's pairing confirmation dialogs will be unavailable (devices "
                "that pair via \"Just Works\" are unaffected)",
                err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        sd_bus_slot_unref(g_agent_vtable_slot);
        g_agent_vtable_slot = NULL;
        return;
    }
    sd_bus_error_free(&err);

    r = sd_bus_call_method(g_system, "org.bluez", "/org/bluez", "org.bluez.AgentManager1",
                           "RequestDefaultAgent", &err, NULL, "o", DC_BLUEZ_AGENT_PATH);
    if (r < 0) {
        dc_warn("bluez: RequestDefaultAgent failed (%s) -- dankc's agent stays registered but "
                "non-default (still used for pairing dankc itself initiates)",
                err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
    } else {
        sd_bus_error_free(&err);
    }
    dc_info("bluez: registered pairing agent (%s)", DC_BLUEZ_AGENT_PATH);
}

bool dc_bluez_agent_poll(dc_bluez_agent_request *out)
{
    if (g_agent.kind == DC_BLUEZ_AGENT_NONE)
        return false;
    out->kind = g_agent.kind;
    snprintf(out->device_name, sizeof(out->device_name), "%s", g_agent.device_name);
    snprintf(out->passkey_str, sizeof(out->passkey_str), "%s", g_agent.passkey_str);
    return true;
}

void dc_bluez_agent_respond_yesno(bool accept)
{
    if (g_agent.fake_pending) {
        dc_info("bluez: [DANKC_BT_DRYRUN] user %s the simulated confirmation",
                accept ? "accepted" : "rejected");
        g_agent.fake_pending = false;
        g_agent.kind = DC_BLUEZ_AGENT_NONE;
        bluez_resolve_fake_pair(accept);
        return;
    }

    if (!g_agent.pending_msg)
        return;
    if (accept)
        sd_bus_reply_method_return(g_agent.pending_msg, "");
    else
        sd_bus_reply_method_errorf(g_agent.pending_msg, "org.bluez.Error.Rejected",
                                   "Rejected by user");
    sd_bus_message_unref(g_agent.pending_msg);
    g_agent.pending_msg = NULL;
    g_agent.kind = DC_BLUEZ_AGENT_NONE;
}

void dc_bluez_agent_respond_passkey(const char *digits)
{
    if (!g_agent.pending_msg)
        return;
    if (digits && digits[0]) {
        uint32_t passkey = (uint32_t)strtoul(digits, NULL, 10);
        sd_bus_reply_method_return(g_agent.pending_msg, "u", passkey);
    } else {
        sd_bus_reply_method_errorf(g_agent.pending_msg, "org.bluez.Error.Rejected", "Cancelled");
    }
    sd_bus_message_unref(g_agent.pending_msg);
    g_agent.pending_msg = NULL;
    g_agent.kind = DC_BLUEZ_AGENT_NONE;
}

/* --- Pairing job (W3.1) -----------------------------------------------------
 */

typedef struct {
    dc_bluez_pair_state state;
    char mac[18];
    char obj_path[128]; /* g_adapter_path (63) + "/dev_" (5) + mac (17) + NUL, rounded up */
    char err[128];

    sd_bus_slot *pair_slot;
    sd_bus_slot *trust_slot;
    sd_bus_slot *connect_slot;

    bool dryrun_pending; /* DANKC_BT_DRYRUN: simulate without real D-Bus calls */
    struct timespec started;
} bluez_pair_job;

static bluez_pair_job g_pair;

static long pair_secs_since(const struct timespec *from)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return now.tv_sec - from->tv_sec;
}

static void mac_to_device_path(const char *mac, char *out, size_t out_sz)
{
    char macu[18];
    size_t j = 0;
    for (const char *p = mac; *p && j < sizeof(macu) - 1; p++)
        macu[j++] = (*p == ':') ? '_' : *p;
    macu[j] = '\0';
    /* Bounded width specifiers (rather than plain "%s") so the compiler can
     * prove this can't overflow `out` regardless of the caller's buffer
     * size, same defensive style as run_bluetoothctl()'s cmd formatting
     * above. */
    snprintf(out, out_sz, "%.63s/dev_%.17s", g_adapter_path[0] ? g_adapter_path : "/org/bluez/hci0",
            macu);
}

static int on_connect_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static void start_connect(void);
static int on_trust_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error);
static void start_trust_and_connect(void);

static void start_trust_and_connect(void)
{
    sd_bus_message *m = NULL;
    int r = sd_bus_message_new_method_call(g_system, &m, "org.bluez", g_pair.obj_path,
                                           "org.freedesktop.DBus.Properties", "Set");
    if (r >= 0) {
        sd_bus_message_append(m, "ss", "org.bluez.Device1", "Trusted");
        sd_bus_message_open_container(m, 'v', "b");
        sd_bus_message_append(m, "b", 1);
        sd_bus_message_close_container(m);
        r = sd_bus_call_async(g_system, &g_pair.trust_slot, m, on_trust_reply, NULL, 0);
        sd_bus_message_unref(m);
    }
    if (r < 0) {
        dc_warn("bluez: Trusted=true request failed to send (%s); connecting anyway",
                strerror(-r));
        start_connect();
    }
}

static int on_trust_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    const sd_bus_error *e = sd_bus_message_get_error(m);
    if (e)
        dc_warn("bluez: Trusted=true failed: %s (continuing to Connect)",
                e->message ? e->message : "?");
    start_connect();
    return 0;
}

static void start_connect(void)
{
    int r = sd_bus_call_method_async(g_system, &g_pair.connect_slot, "org.bluez", g_pair.obj_path,
                                     "org.bluez.Device1", "Connect", on_connect_reply, NULL, "");
    if (r < 0) {
        g_pair.state = DC_BLUEZ_PAIR_FAILED;
        snprintf(g_pair.err, sizeof(g_pair.err), "Connect() call failed: %s", strerror(-r));
    }
}

static int on_connect_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    const sd_bus_error *e = sd_bus_message_get_error(m);
    if (e && !(e->name && strstr(e->name, "AlreadyConnected"))) {
        g_pair.state = DC_BLUEZ_PAIR_FAILED;
        snprintf(g_pair.err, sizeof(g_pair.err), "%s", e->message ? e->message : "Connect failed");
        return 0;
    }
    g_pair.state = DC_BLUEZ_PAIR_SUCCESS;
    g_last_refresh = 0; /* refresh the device list so it flips to paired+connected */
    return 0;
}

static int on_pair_reply(sd_bus_message *m, void *userdata, sd_bus_error *ret_error)
{
    DC_UNUSED(userdata);
    DC_UNUSED(ret_error);
    const sd_bus_error *e = sd_bus_message_get_error(m);
    if (e) {
        /* AlreadyExists == already paired (e.g. a race with another client)
         * -- proceed to Trust+Connect instead of failing outright. */
        bool already_paired = e->name && strstr(e->name, "AlreadyExists");
        if (!already_paired) {
            g_pair.state = DC_BLUEZ_PAIR_FAILED;
            snprintf(g_pair.err, sizeof(g_pair.err), "%s", e->message ? e->message : "Pairing failed");
            return 0;
        }
    }
    start_trust_and_connect();
    return 0;
}

void dc_bluez_pair(const char *mac)
{
    if (!mac || !mac[0])
        return;
    dc_bluez_pair_reset();

    snprintf(g_pair.mac, sizeof(g_pair.mac), "%s", mac);
    g_pair.state = DC_BLUEZ_PAIR_IN_PROGRESS;
    clock_gettime(CLOCK_MONOTONIC, &g_pair.started);

    if (getenv("DANKC_BT_DRYRUN")) {
        dc_info("bluez: [DANKC_BT_DRYRUN] would Pair+Trust+Connect %s", mac);
        g_pair.dryrun_pending = true;

        const char *fake = getenv("DANKC_BT_FAKE_DEVICE");
        if (fake && fake[0] && strcmp(mac, DC_BLUEZ_FAKE_MAC) == 0) {
            /* Simulate BlueZ calling RequestConfirmation mid-pair, so the
             * confirm dialog itself can be exercised/screenshotted without
             * real hardware (docs/14-COMPLETION-PLAN.md W3.1 verification).
             * dc_bluez_agent_respond_yesno() resolves this job once the user
             * answers -- dc_bluez_pair_poll() just waits below. */
            g_agent.kind = DC_BLUEZ_AGENT_CONFIRM;
            snprintf(g_agent.device_name, sizeof(g_agent.device_name), "%s", fake);
            snprintf(g_agent.passkey_str, sizeof(g_agent.passkey_str), "123456");
            g_agent.pending_msg = NULL;
            g_agent.fake_pending = true;
        }
        return;
    }

    if (!g_system || !g_adapter_path[0]) {
        g_pair.state = DC_BLUEZ_PAIR_FAILED;
        snprintf(g_pair.err, sizeof(g_pair.err), "Bluetooth adapter unavailable");
        return;
    }

    mac_to_device_path(mac, g_pair.obj_path, sizeof(g_pair.obj_path));
    int r = sd_bus_call_method_async(g_system, &g_pair.pair_slot, "org.bluez", g_pair.obj_path,
                                     "org.bluez.Device1", "Pair", on_pair_reply, NULL, "");
    if (r < 0) {
        g_pair.state = DC_BLUEZ_PAIR_FAILED;
        snprintf(g_pair.err, sizeof(g_pair.err), "Pair() call failed: %s", strerror(-r));
    }
}

/* Resolves a DANKC_BT_DRYRUN + DANKC_BT_FAKE_DEVICE simulated pairing once
 * the user answers the simulated confirm dialog
 * (dc_bluez_agent_respond_yesno() above). */
static void bluez_resolve_fake_pair(bool ok)
{
    if (!g_pair.dryrun_pending)
        return;
    g_pair.dryrun_pending = false;
    g_pair.state = ok ? DC_BLUEZ_PAIR_SUCCESS : DC_BLUEZ_PAIR_FAILED;
    if (!ok)
        snprintf(g_pair.err, sizeof(g_pair.err), "Rejected by user");
}

dc_bluez_pair_state dc_bluez_pair_poll(char *mac_out, size_t mac_sz, char *err_out, size_t err_sz)
{
    if (g_pair.dryrun_pending && !g_agent.fake_pending) {
        /* Dry-run with no simulated confirm dialog in the way (either no
         * DANKC_BT_FAKE_DEVICE, or a real mac was passed) -- resolve on its
         * own after a short delay, same convention as services/net.c's
         * DANKC_WIFI_DRYRUN. */
        if (pair_secs_since(&g_pair.started) >= 1) {
            g_pair.dryrun_pending = false;
            g_pair.state = DC_BLUEZ_PAIR_SUCCESS;
        }
    }

    if (mac_out)
        snprintf(mac_out, mac_sz, "%s", g_pair.mac);
    if (g_pair.state == DC_BLUEZ_PAIR_FAILED && err_out)
        snprintf(err_out, err_sz, "%s", g_pair.err);
    return g_pair.state;
}

void dc_bluez_pair_reset(void)
{
    if (g_pair.pair_slot)
        sd_bus_slot_unref(g_pair.pair_slot);
    if (g_pair.trust_slot)
        sd_bus_slot_unref(g_pair.trust_slot);
    if (g_pair.connect_slot)
        sd_bus_slot_unref(g_pair.connect_slot);
    memset(&g_pair, 0, sizeof(g_pair));
    g_pair.state = DC_BLUEZ_PAIR_IDLE;

    /* Never leave BlueZ (or the simulated dry-run dialog) waiting on a reply
     * we'll now never send. */
    if (g_agent.pending_msg) {
        sd_bus_reply_method_errorf(g_agent.pending_msg, "org.bluez.Error.Canceled", "Cancelled");
        sd_bus_message_unref(g_agent.pending_msg);
        g_agent.pending_msg = NULL;
    }
    g_agent.kind = DC_BLUEZ_AGENT_NONE;
    g_agent.fake_pending = false;
}
