#include "services/bluez.h"

#include "core/log.h"
#include "services/dbus.h"

#include <string.h>
#include <time.h>

#define DC_BLUEZ_REFRESH_SECS 3

static sd_bus *g_system = NULL;
static dc_bluez_info g_cache;
static time_t g_last_refresh = 0;

void dc_bluez_init(struct dc_dbus *dbus)
{
    g_system = dbus ? dbus->system : NULL;
}

/* Scan GetManagedObjects for a powered adapter and any connected device. */
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

        if (sd_bus_message_enter_container(reply, 'a', "{sa{sv}}") < 0) {
            sd_bus_message_exit_container(reply);
            break;
        }
        while (sd_bus_message_enter_container(reply, 'e', "sa{sv}") > 0) {
            const char *iface = NULL;
            sd_bus_message_read_basic(reply, 's', &iface);

            bool is_device = iface && strcmp(iface, "org.bluez.Device1") == 0;
            bool is_adapter = iface && strcmp(iface, "org.bluez.Adapter1") == 0;

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
                        if (connected)
                            info->connected = true;
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
    }
    sd_bus_message_exit_container(reply); /* a{oa{sa{sv}}} */

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
