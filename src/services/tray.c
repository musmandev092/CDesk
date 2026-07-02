#include "services/tray.h"

#include "core/log.h"
#include "dc.h"
#include "services/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DC_SNW_NAME "org.kde.StatusNotifierWatcher"
#define DC_SNW_PATH "/StatusNotifierWatcher"
#define DC_SNW_IFACE "org.kde.StatusNotifierWatcher"
#define DC_SNI_IFACE "org.kde.StatusNotifierItem"

struct dc_tray {
    sd_bus *bus;
    sd_bus_slot *reg_slot;
    sd_bus_slot *unreg_slot;
    dc_tray_item items[DC_TRAY_MAX];
    int count;
    dc_tray_changed_cb cb;
    void *cb_data;
};

/* Split a watcher entry ("service" or "service/path") into service + path. */
static void split_entry(const char *entry, char *service, char *path, size_t n)
{
    const char *slash = strchr(entry, '/');
    if (slash) {
        size_t sl = (size_t)(slash - entry);
        if (sl >= n)
            sl = n - 1;
        memcpy(service, entry, sl);
        service[sl] = '\0';
        snprintf(path, n, "%s", slash);
    } else {
        snprintf(service, n, "%s", entry);
        snprintf(path, n, "%s", "/StatusNotifierItem");
    }
}

static void read_item_props(dc_tray *t, dc_tray_item *item)
{
    char *icon = NULL, *title = NULL;
    if (sd_bus_get_property_string(t->bus, item->service, item->path, DC_SNI_IFACE, "IconName",
                                   NULL, &icon) >= 0 &&
        icon) {
        snprintf(item->icon_name, sizeof(item->icon_name), "%s", icon);
        free(icon);
    }
    if (sd_bus_get_property_string(t->bus, item->service, item->path, DC_SNI_IFACE, "Title", NULL,
                                   &title) >= 0 &&
        title) {
        snprintf(item->title, sizeof(item->title), "%s", title);
        free(title);
    }
}

/* Re-query the watcher's registered items and their icon/title. */
static void refresh(dc_tray *t)
{
    t->count = 0;

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_get_property(t->bus, DC_SNW_NAME, DC_SNW_PATH, DC_SNW_IFACE,
                               "RegisteredStatusNotifierItems", &err, &reply, "as");
    if (r < 0) {
        sd_bus_error_free(&err);
        return;
    }

    sd_bus_message_enter_container(reply, 'a', "s");
    const char *entry = NULL;
    while (sd_bus_message_read_basic(reply, 's', &entry) > 0 && t->count < DC_TRAY_MAX) {
        dc_tray_item *item = &t->items[t->count];
        memset(item, 0, sizeof(*item));
        split_entry(entry, item->service, item->path, sizeof(item->service));
        read_item_props(t, item);
        t->count++;
    }
    sd_bus_message_exit_container(reply);
    sd_bus_message_unref(reply);

    dc_debug("tray: %d item(s)", t->count);
    if (t->cb)
        t->cb(t->cb_data);
}

static int on_items_changed(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    DC_UNUSED(msg);
    DC_UNUSED(err);
    refresh(userdata);
    return 0;
}

dc_tray *dc_tray_create(struct dc_dbus *dbus)
{
    if (!dbus || !dbus->user)
        return NULL;

    dc_tray *t = calloc(1, sizeof(*t));
    t->bus = dbus->user;

    /* Refresh whenever the watcher gains or loses an item. */
    sd_bus_match_signal(t->bus, &t->reg_slot, DC_SNW_NAME, DC_SNW_PATH, DC_SNW_IFACE,
                        "StatusNotifierItemRegistered", on_items_changed, t);
    sd_bus_match_signal(t->bus, &t->unreg_slot, DC_SNW_NAME, DC_SNW_PATH, DC_SNW_IFACE,
                        "StatusNotifierItemUnregistered", on_items_changed, t);

    refresh(t);
    dc_info("tray host reading %s", DC_SNW_NAME);
    return t;
}

void dc_tray_destroy(dc_tray *t)
{
    if (!t)
        return;
    sd_bus_slot_unref(t->reg_slot);
    sd_bus_slot_unref(t->unreg_slot);
    free(t);
}

void dc_tray_set_changed_cb(dc_tray *t, dc_tray_changed_cb cb, void *user_data)
{
    if (!t)
        return;
    t->cb = cb;
    t->cb_data = user_data;
}

int dc_tray_items(dc_tray *t, const dc_tray_item **out, int max)
{
    if (!t)
        return 0;
    int n = t->count < max ? t->count : max;
    for (int i = 0; i < n; i++)
        out[i] = &t->items[i];
    return n;
}
