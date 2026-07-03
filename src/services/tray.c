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

    /* "Menu" is an OBJECT_PATH property ('o'), not a string ('s') --
     * sd_bus_get_property_string() requests contents signature "s" and would
     * fail the type check, so fetch it directly. Absent on items with no
     * dbusmenu (right-click falls back to ContextMenu -- ui/tray_menu.c). */
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    if (sd_bus_get_property(t->bus, item->service, item->path, DC_SNI_IFACE, "Menu", &err, &reply,
                            "o") >= 0) {
        const char *menu_path = NULL;
        if (sd_bus_message_read_basic(reply, 'o', &menu_path) >= 0 && menu_path)
            snprintf(item->menu_path, sizeof(item->menu_path), "%s", menu_path);
    }
    sd_bus_error_free(&err);
    if (reply)
        sd_bus_message_unref(reply);
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

/* --- click-to-activate (docs/POLISH.md P4) -------------------------------- */

/* Fire-and-forget call of a no-reply-value SNI method on item `index`
 * (same "ii" x,y signature for Activate/SecondaryActivate/ContextMenu). */
static void call_item_method(dc_tray *t, int index, const char *method, int x, int y)
{
    if (!t || index < 0 || index >= t->count)
        return;
    dc_tray_item *item = &t->items[index];

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_call_method(t->bus, item->service, item->path, DC_SNI_IFACE, method, &err,
                               &reply, "ii", x, y);
    if (r < 0) {
        dc_debug("tray: %s failed on %s: %s", method, item->service,
                err.message ? err.message : "?");
        sd_bus_error_free(&err);
    }
    if (reply)
        sd_bus_message_unref(reply);
}

void dc_tray_activate(dc_tray *t, int index, int x, int y)
{
    call_item_method(t, index, "Activate", x, y);
}

void dc_tray_secondary_activate(dc_tray *t, int index, int x, int y)
{
    call_item_method(t, index, "SecondaryActivate", x, y);
}

void dc_tray_context_menu(dc_tray *t, int index, int x, int y)
{
    call_item_method(t, index, "ContextMenu", x, y);
}

/* --- IconPixmap fallback (docs/POLISH.md P4) ------------------------------ */

bool dc_tray_icon_pixmap(dc_tray *t, int index, int max_dim, uint8_t **out_rgba, int *out_w,
                         int *out_h)
{
    *out_rgba = NULL;
    *out_w = 0;
    *out_h = 0;
    if (!t || index < 0 || index >= t->count)
        return false;
    dc_tray_item *item = &t->items[index];

    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    int r = sd_bus_get_property(t->bus, item->service, item->path, DC_SNI_IFACE, "IconPixmap",
                                &err, &reply, "a(iiay)");
    if (r < 0) {
        sd_bus_error_free(&err);
        return false;
    }

    /* Pick the largest variant with both dims <= max_dim; if none qualify,
     * fall back to the smallest available (still renders, just softer). */
    int32_t best_w = 0, best_h = 0;
    const void *best_data = NULL;
    bool best_fits = false;

    if (sd_bus_message_enter_container(reply, 'a', "(iiay)") > 0) {
        while (sd_bus_message_enter_container(reply, 'r', "iiay") > 0) {
            int32_t w = 0, h = 0;
            sd_bus_message_read(reply, "ii", &w, &h);
            const void *data = NULL;
            size_t len = 0;
            sd_bus_message_read_array(reply, 'y', &data, &len);
            sd_bus_message_exit_container(reply); /* r */

            if (w <= 0 || h <= 0 || (size_t)w * (size_t)h * 4 > len)
                continue; /* malformed/short -- skip rather than read OOB */

            bool fits = w <= max_dim && h <= max_dim;
            bool pick;
            if (!best_data)
                pick = true;
            else if (fits != best_fits)
                pick = fits; /* prefer any in-budget variant over an oversized one */
            else if (fits)
                pick = w > best_w; /* both fit: prefer the larger (sharper) */
            else
                pick = w < best_w; /* both oversized: prefer the smaller (less downscale) */

            if (pick) {
                best_w = w;
                best_h = h;
                best_data = data;
                best_fits = fits;
            }
        }
        sd_bus_message_exit_container(reply); /* a */
    }

    bool ok = false;
    if (best_data) {
        uint8_t *rgba = malloc((size_t)best_w * (size_t)best_h * 4);
        if (rgba) {
            const uint8_t *src = best_data;
            const int64_t n = (int64_t)best_w * (int64_t)best_h;
            /* ARGB32, network (big-endian) byte order: byte0=A,1=R,2=G,3=B.
             * nanovg/nvgCreateImageRGBA wants straight (non-premultiplied)
             * RGBA8 -- same convention as every other nvgCreateImageRGBA call
             * in this codebase (flags=0; see ui/notif_image.c). */
            for (int64_t i = 0; i < n; i++) {
                uint8_t a = src[i * 4 + 0], r_ = src[i * 4 + 1], g = src[i * 4 + 2],
                        b = src[i * 4 + 3];
                rgba[i * 4 + 0] = r_;
                rgba[i * 4 + 1] = g;
                rgba[i * 4 + 2] = b;
                rgba[i * 4 + 3] = a;
            }
            *out_rgba = rgba;
            *out_w = (int)best_w;
            *out_h = (int)best_h;
            ok = true;
        }
    }

    sd_bus_message_unref(reply);
    return ok;
}
