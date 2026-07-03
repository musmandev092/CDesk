#include "services/bluez.h"

#include "core/log.h"
#include "services/dbus.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define DC_BLUEZ_REFRESH_SECS 3

static sd_bus *g_system = NULL;
static dc_bluez_info g_cache;
static time_t g_last_refresh = 0;

void dc_bluez_init(struct dc_dbus *dbus)
{
    g_system = dbus ? dbus->system : NULL;
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
         * as they stream by is simplest). */
        bool is_device_obj = false;
        bool dev_paired = false, dev_connected = false;
        char dev_name[64] = {0};
        char dev_alias[64] = {0};

        if (sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") < 0) {
            sd_bus_message_exit_container(reply);
            break;
        }
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *iface = NULL;
            sd_bus_message_read_basic(reply, 's', &iface);

            bool is_device = iface && strcmp(iface, "org.bluez.Device1") == 0;
            bool is_adapter = iface && strcmp(iface, "org.bluez.Adapter1") == 0;
            if (is_device)
                is_device_obj = true;

            if (is_device || is_adapter) {
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
                    } else if (is_adapter && prop && strcmp(prop, "Powered") == 0) {
                        int powered = 0;
                        sd_bus_message_enter_container(reply, 'v', "b");
                        sd_bus_message_read_basic(reply, 'b', &powered);
                        sd_bus_message_exit_container(reply);
                        if (powered)
                            info->powered = true;
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

        /* Only surface devices worth showing: paired (the "known devices"
         * list) or currently connected. Unpaired nearby-scan noise is
         * skipped -- BlueZ only reports it while a discovery is active
         * anyway, which dankc doesn't start. */
        if (is_device_obj && (dev_paired || dev_connected) &&
            info->device_count < DC_BLUEZ_MAX_DEVICES) {
            dc_bluez_device *d = &info->devices[info->device_count];
            memset(d, 0, sizeof(*d));
            mac_from_path(path, d->mac, sizeof(d->mac));
            const char *name = dev_name[0] ? dev_name : (dev_alias[0] ? dev_alias : d->mac);
            snprintf(d->name, sizeof(d->name), "%s", name);
            d->paired = dev_paired;
            d->connected = dev_connected;
            info->device_count++;
        }
    }

done:
    sd_bus_message_unref(reply);
}

bool dc_bluez_read(dc_bluez_info *out)
{
    time_t now = time(NULL);
    if (g_last_refresh != 0 && now - g_last_refresh < DC_BLUEZ_REFRESH_SECS) {
        *out = g_cache;
        return g_cache.available;
    }
    g_last_refresh = now;

    memset(&g_cache, 0, sizeof(g_cache));
    if (g_system)
        scan_objects(&g_cache);

    *out = g_cache;
    return g_cache.available;
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
