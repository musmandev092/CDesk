#include "services/notifications.h"

#include "core/log.h"
#include "services/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DC_NOTIF_PATH "/org/freedesktop/Notifications"
#define DC_NOTIF_IFACE "org.freedesktop.Notifications"
#define DC_NOTIF_NAME "org.freedesktop.Notifications"

/* Default on-screen lifetime by urgency when the app passes expire_timeout=-1,
 * matching DMS: low/normal auto-dismiss, critical stays until acted on. */
#define DC_NOTIF_DEFAULT_MS 5000
#define DC_NOTIF_LOW_MS 5000

/* Closed-notification reasons (Desktop Notifications spec). */
#define DC_NOTIF_REASON_EXPIRED 1
#define DC_NOTIF_REASON_DISMISSED 2
#define DC_NOTIF_REASON_CLOSED 3

#define DC_NOTIF_HISTORY 64

struct dc_notifications {
    sd_bus *bus;
    sd_bus_slot *slot;
    dc_notification items[DC_NOTIF_MAX];
    uint32_t next_id;
    dc_notif_changed_cb changed_cb;
    void *changed_data;

    dc_notification history[DC_NOTIF_HISTORY]; /* ring buffer, oldest at head */
    int history_count;
    int history_head;

    bool has_unread; /* bar bell dot (docs/12-BAR-SPEC.md sec.4/6) */
};

/* Copy a notification into the history ring, evicting the oldest when full. */
static void push_history(dc_notifications *n, const dc_notification *item)
{
    int idx;
    if (n->history_count < DC_NOTIF_HISTORY) {
        idx = (n->history_head + n->history_count) % DC_NOTIF_HISTORY;
        n->history_count++;
    } else {
        idx = n->history_head;
        n->history_head = (n->history_head + 1) % DC_NOTIF_HISTORY;
    }
    n->history[idx] = *item;
    n->history[idx].popup = false;
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void notify_changed(dc_notifications *n)
{
    if (n->changed_cb)
        n->changed_cb(n->changed_data);
}

/* Find the slot holding `id`, or NULL. */
static dc_notification *find_by_id(dc_notifications *n, uint32_t id)
{
    for (int i = 0; i < DC_NOTIF_MAX; i++)
        if (n->items[i].active && n->items[i].id == id)
            return &n->items[i];
    return NULL;
}

/* Reuse the slot for `replaces_id`, else the oldest free/expired slot. */
static dc_notification *acquire_slot(dc_notifications *n, uint32_t replaces_id)
{
    if (replaces_id != 0) {
        dc_notification *existing = find_by_id(n, replaces_id);
        if (existing)
            return existing;
    }
    for (int i = 0; i < DC_NOTIF_MAX; i++)
        if (!n->items[i].active)
            return &n->items[i];

    /* Full: evict the oldest. */
    dc_notification *oldest = &n->items[0];
    for (int i = 1; i < DC_NOTIF_MAX; i++)
        if (n->items[i].created_ms < oldest->created_ms)
            oldest = &n->items[i];
    return oldest;
}

static void emit_closed(dc_notifications *n, uint32_t id, uint32_t reason)
{
    sd_bus_emit_signal(n->bus, DC_NOTIF_PATH, DC_NOTIF_IFACE, "NotificationClosed", "uu", id,
                       reason);
}

/* --- D-Bus methods ------------------------------------------------------- */

static int method_notify(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    (void)err;
    dc_notifications *n = userdata;

    const char *app_name = NULL, *app_icon = NULL, *summary = NULL, *body = NULL;
    uint32_t replaces_id = 0;
    int32_t expire_timeout = -1;

    int r = sd_bus_message_read(msg, "susss", &app_name, &replaces_id, &app_icon, &summary, &body);
    if (r < 0)
        return r;

    /* actions: as — skip (action buttons land in a later milestone). */
    sd_bus_message_skip(msg, "as");

    /* hints: a{sv} — pull out urgency (byte) and image-path/app-icon strings. */
    dc_urgency urgency = DC_URGENCY_NORMAL;
    const char *hint_icon = NULL;
    r = sd_bus_message_enter_container(msg, 'a', "{sv}");
    if (r >= 0) {
        while (sd_bus_message_enter_container(msg, 'e', "sv") > 0) {
            const char *key = NULL;
            sd_bus_message_read_basic(msg, 's', &key);
            if (key && strcmp(key, "urgency") == 0) {
                uint8_t u = 1;
                sd_bus_message_enter_container(msg, 'v', "y");
                sd_bus_message_read_basic(msg, 'y', &u);
                sd_bus_message_exit_container(msg);
                urgency = (u <= DC_URGENCY_CRITICAL) ? (dc_urgency)u : DC_URGENCY_NORMAL;
            } else if (key && (strcmp(key, "image-path") == 0 ||
                               strcmp(key, "image_path") == 0)) {
                sd_bus_message_enter_container(msg, 'v', "s");
                sd_bus_message_read_basic(msg, 's', &hint_icon);
                sd_bus_message_exit_container(msg);
            } else {
                sd_bus_message_skip(msg, "v");
            }
            sd_bus_message_exit_container(msg); /* sv */
        }
        sd_bus_message_exit_container(msg); /* a{sv} */
    }

    sd_bus_message_read(msg, "i", &expire_timeout);

    uint32_t id = replaces_id != 0 ? replaces_id : ++n->next_id;
    dc_notification *slot = acquire_slot(n, replaces_id);
    if (slot->active && slot->id != id) {
        push_history(n, slot); /* evicted a different live notification */
        emit_closed(n, slot->id, DC_NOTIF_REASON_CLOSED);
    }

    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->urgency = urgency;
    slot->expire_timeout_ms = expire_timeout; /* -1 default, 0 never, >0 ms */
    slot->created_ms = now_ms();
    slot->popup = true;
    slot->active = true;
    snprintf(slot->app_name, sizeof(slot->app_name), "%s", app_name ? app_name : "");
    snprintf(slot->summary, sizeof(slot->summary), "%s", summary ? summary : "");
    snprintf(slot->body, sizeof(slot->body), "%s", body ? body : "");
    snprintf(slot->app_icon, sizeof(slot->app_icon), "%s",
             (app_icon && *app_icon) ? app_icon : (hint_icon ? hint_icon : ""));

    dc_info("notify #%u [%s] %s", id, slot->app_name, slot->summary);
    n->has_unread = true;
    notify_changed(n);

    return sd_bus_reply_method_return(msg, "u", id);
}

static int method_close(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    (void)err;
    dc_notifications *n = userdata;
    uint32_t id = 0;
    int r = sd_bus_message_read(msg, "u", &id);
    if (r < 0)
        return r;

    dc_notification *slot = find_by_id(n, id);
    if (slot) {
        push_history(n, slot);
        slot->active = false;
        emit_closed(n, id, DC_NOTIF_REASON_CLOSED);
        notify_changed(n);
    }
    return sd_bus_reply_method_return(msg, "");
}

static int method_get_capabilities(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    (void)err;
    return sd_bus_reply_method_return(msg, "as", 3, "body", "body-markup", "persistence");
}

static int method_get_server_information(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    (void)err;
    return sd_bus_reply_method_return(msg, "ssss", "DankC", "danklinux", "0.1", "1.2");
}

static const sd_bus_vtable notif_vtable[] = {
    SD_BUS_VTABLE_START(0),
    SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", method_notify, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("CloseNotification", "u", "", method_close, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetCapabilities", "", "as", method_get_capabilities, SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_METHOD("GetServerInformation", "", "ssss", method_get_server_information,
                  SD_BUS_VTABLE_UNPRIVILEGED),
    SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
    SD_BUS_SIGNAL("ActionInvoked", "us", 0),
    SD_BUS_VTABLE_END,
};

/* --- lifecycle ----------------------------------------------------------- */

dc_notifications *dc_notifications_create(struct dc_dbus *dbus)
{
    if (!dbus || !dbus->user) {
        dc_warn("no session bus; notification server disabled");
        return NULL;
    }

    dc_notifications *n = calloc(1, sizeof(*n));
    n->bus = dbus->user;

    int r = sd_bus_add_object_vtable(n->bus, &n->slot, DC_NOTIF_PATH, DC_NOTIF_IFACE, notif_vtable,
                                     n);
    if (r < 0) {
        dc_warn("sd_bus_add_object_vtable failed: %s", strerror(-r));
        free(n);
        return NULL;
    }

    r = sd_bus_request_name(n->bus, DC_NOTIF_NAME, 0);
    if (r < 0) {
        dc_warn("could not own %s (another daemon running?): %s", DC_NOTIF_NAME, strerror(-r));
        sd_bus_slot_unref(n->slot);
        free(n);
        return NULL;
    }

    dc_info("notification server registered (%s)", DC_NOTIF_NAME);
    return n;
}

void dc_notifications_destroy(dc_notifications *n)
{
    if (!n)
        return;
    sd_bus_release_name(n->bus, DC_NOTIF_NAME);
    sd_bus_slot_unref(n->slot);
    free(n);
}

void dc_notifications_set_changed_cb(dc_notifications *n, dc_notif_changed_cb cb, void *user_data)
{
    if (!n)
        return;
    n->changed_cb = cb;
    n->changed_data = user_data;
}

/* Milliseconds a toast should remain on-screen given its urgency/timeout. */
static int lifetime_ms(const dc_notification *item)
{
    if (item->expire_timeout_ms > 0)
        return item->expire_timeout_ms;
    if (item->expire_timeout_ms == 0)
        return 0; /* never auto-expire */
    /* server default (-1) */
    if (item->urgency == DC_URGENCY_CRITICAL)
        return 0;
    if (item->urgency == DC_URGENCY_LOW)
        return DC_NOTIF_LOW_MS;
    return DC_NOTIF_DEFAULT_MS;
}

bool dc_notifications_tick(dc_notifications *n)
{
    if (!n)
        return false;
    bool changed = false;
    int64_t t = now_ms();
    for (int i = 0; i < DC_NOTIF_MAX; i++) {
        dc_notification *item = &n->items[i];
        if (!item->active || !item->popup)
            continue;
        int life = lifetime_ms(item);
        if (life > 0 && t - item->created_ms >= life) {
            push_history(n, item);
            item->popup = false;
            item->active = false;
            emit_closed(n, item->id, DC_NOTIF_REASON_EXPIRED);
            changed = true;
        }
    }
    if (changed)
        notify_changed(n);
    return changed;
}

int dc_notifications_popups(dc_notifications *n, const dc_notification **out, int max)
{
    if (!n)
        return 0;
    int count = 0;
    /* Newest first: scan by descending created_ms via simple selection. */
    int64_t last = INT64_MAX;
    for (int picked = 0; picked < max; picked++) {
        dc_notification *best = NULL;
        for (int i = 0; i < DC_NOTIF_MAX; i++) {
            dc_notification *item = &n->items[i];
            if (!item->active || !item->popup)
                continue;
            if (item->created_ms < last &&
                (best == NULL || item->created_ms > best->created_ms))
                best = item;
        }
        if (!best)
            break;
        out[count++] = best;
        last = best->created_ms;
    }
    return count;
}

void dc_notifications_dismiss(dc_notifications *n, uint32_t id)
{
    if (!n)
        return;
    dc_notification *slot = find_by_id(n, id);
    if (!slot)
        return;
    push_history(n, slot);
    slot->popup = false;
    slot->active = false;
    emit_closed(n, id, DC_NOTIF_REASON_DISMISSED);
    notify_changed(n);
}

int dc_notifications_history(dc_notifications *n, const dc_notification **out, int max)
{
    if (!n || max <= 0)
        return 0;
    int count = 0;
    /* Newest first: walk the ring backwards from the most recent entry. */
    for (int i = n->history_count - 1; i >= 0 && count < max; i--) {
        int idx = (n->history_head + i) % DC_NOTIF_HISTORY;
        out[count++] = &n->history[idx];
    }
    return count;
}

int dc_notifications_history_count(dc_notifications *n)
{
    return n ? n->history_count : 0;
}

void dc_notifications_clear_history(dc_notifications *n)
{
    if (!n)
        return;
    n->history_count = 0;
    n->history_head = 0;
    notify_changed(n);
}

bool dc_notifications_has_unread(dc_notifications *n)
{
    return n && n->has_unread;
}

void dc_notifications_mark_read(dc_notifications *n)
{
    if (!n)
        return;
    n->has_unread = false;
}
