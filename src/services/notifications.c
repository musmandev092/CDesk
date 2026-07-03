#include "services/notifications.h"

#include "core/config.h"
#include "core/log.h"
#include "services/dbus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define DC_NOTIF_PATH "/org/freedesktop/Notifications"
#define DC_NOTIF_IFACE "org.freedesktop.Notifications"
#define DC_NOTIF_NAME "org.freedesktop.Notifications"

/* Closed-notification reasons (Desktop Notifications spec). */
#define DC_NOTIF_REASON_EXPIRED 1
#define DC_NOTIF_REASON_DISMISSED 2
#define DC_NOTIF_REASON_CLOSED 3

struct dc_notifications {
    sd_bus *bus;
    sd_bus_slot *slot;
    dc_notification store[DC_NOTIF_MAX]; /* both Current and History tabs */
    uint32_t next_id;
    uint32_t next_image_ver; /* monotonic counter for dc_notification.image_version */
    dc_notif_changed_cb changed_cb;
    void *changed_data;

    bool has_unread; /* bar bell dot (docs/12-BAR-SPEC.md sec.4/6) */
};

static int select_by_status(dc_notifications *n, dc_notif_status status, const dc_notification **out,
                            int max);
static void enforce_history_image_cap(dc_notifications *n);

/* Release a slot's decoded inline-image pixels (if any) -- must run before
 * every memset(slot, 0, ...) and before a slot's active flag is dropped,
 * otherwise the malloc'd buffer is orphaned for the process's lifetime. */
static void free_slot_image(dc_notification *slot)
{
    free(slot->image_pixels);
    slot->image_pixels = NULL;
    slot->image_w = slot->image_h = 0;
}

/* --- image-data hint decoding --------------------------------------------- */

/* Box-downsample a tightly-packed RGBA8 buffer from (sw,sh) to (dw,dh) -- see
 * DC_NOTIF_IMAGE_MAX_DIM's comment: called only when a decoded image-data
 * payload exceeds the cap on either side. Each destination pixel is the
 * average of its corresponding source block (smoother than nearest-neighbor
 * and cheap enough at these tiny target sizes -- worst case is 128x128). */
static void box_downscale_rgba(const unsigned char *src, int sw, int sh, unsigned char *dst, int dw,
                               int dh)
{
    for (int y = 0; y < dh; y++) {
        int sy0 = (int)((int64_t)y * sh / dh);
        int sy1 = (int)((int64_t)(y + 1) * sh / dh);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        if (sy1 > sh)
            sy1 = sh;
        for (int x = 0; x < dw; x++) {
            int sx0 = (int)((int64_t)x * sw / dw);
            int sx1 = (int)((int64_t)(x + 1) * sw / dw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            if (sx1 > sw)
                sx1 = sw;
            long sum[4] = {0, 0, 0, 0};
            int cnt = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const unsigned char *row = src + (size_t)yy * sw * 4;
                for (int xx = sx0; xx < sx1; xx++) {
                    const unsigned char *px = row + (size_t)xx * 4;
                    sum[0] += px[0];
                    sum[1] += px[1];
                    sum[2] += px[2];
                    sum[3] += px[3];
                    cnt++;
                }
            }
            unsigned char *o = dst + ((size_t)y * dw + x) * 4;
            o[0] = (unsigned char)(sum[0] / cnt);
            o[1] = (unsigned char)(sum[1] / cnt);
            o[2] = (unsigned char)(sum[2] / cnt);
            o[3] = (unsigned char)(sum[3] / cnt);
        }
    }
}

/* Decode the Notify() "image-data"/"image_data"/"icon_data" hint (iiibiiay:
 * width, height, rowstride, has_alpha, bits_per_sample, channels, pixel
 * bytes) into a tightly-packed RGBA8 buffer, downscaled to fit
 * DC_NOTIF_IMAGE_MAX_DIM on the long side if needed (see that macro's
 * comment). Only 8-bit-per-sample RGB/RGBA payloads are supported -- what
 * every real sender emits (GTK, libnotify, dunst's own tooling); anything
 * else is rejected rather than guessed at. Returns false (leaving *out_pixels
 * untouched) if the payload doesn't validate; most importantly if `data_len`
 * is too small for the claimed geometry, since rowstride/width/height come
 * straight off the bus and are the one thing standing between a
 * malicious/buggy sender and an out-of-bounds read. */
static bool decode_image_data(int32_t width, int32_t height, int32_t rowstride, bool has_alpha,
                              int32_t bps, int32_t channels, const uint8_t *data, size_t data_len,
                              unsigned char **out_pixels, int *out_w, int *out_h)
{
    if (width <= 0 || height <= 0 || width > 4096 || height > 4096 || !data)
        return false;
    if (bps != 8 || (channels != 3 && channels != 4))
        return false;
    if (has_alpha && channels != 4)
        return false;
    int64_t need_row = (int64_t)width * channels;
    if (rowstride < need_row)
        return false;
    int64_t need_total = (int64_t)rowstride * (height - 1) + need_row;
    if ((int64_t)data_len < need_total)
        return false;

    unsigned char *rgba = malloc((size_t)width * (size_t)height * 4);
    if (!rgba)
        return false;
    for (int y = 0; y < height; y++) {
        const uint8_t *row = data + (size_t)y * (size_t)rowstride;
        unsigned char *orow = rgba + (size_t)y * (size_t)width * 4;
        for (int x = 0; x < width; x++) {
            const uint8_t *px = row + (size_t)x * (size_t)channels;
            orow[x * 4 + 0] = px[0];
            orow[x * 4 + 1] = px[1];
            orow[x * 4 + 2] = px[2];
            orow[x * 4 + 3] = channels == 4 ? px[3] : 255;
        }
    }

    int dw = width, dh = height;
    if (dw > DC_NOTIF_IMAGE_MAX_DIM || dh > DC_NOTIF_IMAGE_MAX_DIM) {
        float scale = (float)DC_NOTIF_IMAGE_MAX_DIM / (float)(dw > dh ? dw : dh);
        dw = (int)((float)dw * scale);
        dh = (int)((float)dh * scale);
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
        unsigned char *small = malloc((size_t)dw * (size_t)dh * 4);
        if (!small) {
            free(rgba);
            return false;
        }
        box_downscale_rgba(rgba, width, height, small, dw, dh);
        free(rgba);
        rgba = small;
    }

    *out_pixels = rgba;
    *out_w = dw;
    *out_h = dh;
    return true;
}

/* Memory bound for History (DC_NOTIF_HISTORY_IMAGE_KEEP): free decoded
 * image-data pixels from every archived History entry except the N most
 * recently-created ones. Run from notify_changed() so it self-heals after
 * every state change regardless of which call path (dismiss/clear/tick) just
 * moved something into History -- newest-first selection scan, same style as
 * select_by_status() above (store is small so this is cheap). Current-tab and
 * path/file-based images (image_path/app_icon, just strings) are never
 * touched: only History entries holding decoded image-data pixels count
 * against the cap. */
static void enforce_history_image_cap(dc_notifications *n)
{
    dc_notification *keep[DC_NOTIF_MAX];
    int count = 0;
    int64_t last = INT64_MAX;
    for (int picked = 0; picked < DC_NOTIF_MAX; picked++) {
        dc_notification *best = NULL;
        for (int i = 0; i < DC_NOTIF_MAX; i++) {
            dc_notification *s = &n->store[i];
            if (!s->active || s->status != DC_NOTIF_HISTORY || !s->image_pixels)
                continue;
            if (s->created_ms < last && (!best || s->created_ms > best->created_ms))
                best = s;
        }
        if (!best)
            break;
        keep[count++] = best;
        last = best->created_ms;
    }
    for (int i = DC_NOTIF_HISTORY_IMAGE_KEEP; i < count; i++)
        free_slot_image(keep[i]);
}

static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Wall-clock counterpart of now_ms(), used only for the notification center's
 * "app-name • time" label -- CLOCK_MONOTONIC has no fixed epoch (often boot
 * time), so it can't be formatted as a calendar date/time. */
static int64_t now_wall_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void notify_changed(dc_notifications *n)
{
    /* Single choke point for every call path that can move a notification
     * into (or leave it in) History, so the memory cap self-heals regardless
     * of which one fired -- see enforce_history_image_cap()'s own comment. */
    enforce_history_image_cap(n);
    if (n->changed_cb)
        n->changed_cb(n->changed_data);
}

/* Find the slot holding `id` (Current or History), or NULL. */
static dc_notification *find_by_id(dc_notifications *n, uint32_t id)
{
    for (int i = 0; i < DC_NOTIF_MAX; i++)
        if (n->store[i].active && n->store[i].id == id)
            return &n->store[i];
    return NULL;
}

/* Reuse the slot for `replaces_id` if it still exists; else the first free
 * slot; else evict to make room -- preferring to evict a History entry (it's
 * already been acted on) over a still-Current one. */
static dc_notification *acquire_slot(dc_notifications *n, uint32_t replaces_id)
{
    if (replaces_id != 0) {
        dc_notification *existing = find_by_id(n, replaces_id);
        if (existing)
            return existing;
    }
    for (int i = 0; i < DC_NOTIF_MAX; i++)
        if (!n->store[i].active)
            return &n->store[i];

    dc_notification *oldest_history = NULL;
    dc_notification *oldest_any = &n->store[0];
    for (int i = 0; i < DC_NOTIF_MAX; i++) {
        dc_notification *s = &n->store[i];
        if (s->created_ms < oldest_any->created_ms)
            oldest_any = s;
        if (s->status == DC_NOTIF_HISTORY &&
            (!oldest_history || s->created_ms < oldest_history->created_ms))
            oldest_history = s;
    }
    return oldest_history ? oldest_history : oldest_any;
}

static void emit_closed(dc_notifications *n, uint32_t id, uint32_t reason)
{
    sd_bus_emit_signal(n->bus, DC_NOTIF_PATH, DC_NOTIF_IFACE, "NotificationClosed", "uu", id,
                       reason);
}

/* Resolve a per-card dismiss/action against the Current/History mapping
 * (notifications.h): a Current notification is still "open" as far as the
 * sending app knows, so this is what actually closes it (emits
 * NotificationClosed, matching the desktop-notifications spec); a History
 * notification was already closed once, so acting on it again just forgets
 * it locally -- no second signal. */
static void resolve_dismiss(dc_notifications *n, dc_notification *slot)
{
    if (slot->status == DC_NOTIF_CURRENT) {
        slot->status = DC_NOTIF_HISTORY;
        slot->popup = false;
        emit_closed(n, slot->id, DC_NOTIF_REASON_DISMISSED);
    } else {
        slot->active = false; /* forget it entirely */
        free_slot_image(slot);
    }
    notify_changed(n);
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

    /* actions: as -- a flat [key1, label1, key2, label2, ...] array. The
     * spec's reserved "default" key (invoked by clicking the card body, not a
     * button -- see notifcenter.c/toasts.c click handling) is filtered out;
     * everything else becomes a real button, capped at DC_NOTIF_ACTION_MAX
     * (see that macro's comment). */
    dc_notif_action parsed_actions[DC_NOTIF_ACTION_MAX];
    int action_count = 0;
    r = sd_bus_message_enter_container(msg, 'a', "s");
    if (r >= 0) {
        const char *key = NULL, *label = NULL;
        while (sd_bus_message_read_basic(msg, 's', &key) > 0) {
            label = NULL;
            if (sd_bus_message_read_basic(msg, 's', &label) <= 0)
                label = "";
            if (key && strcmp(key, "default") == 0)
                continue;
            if (action_count < DC_NOTIF_ACTION_MAX) {
                snprintf(parsed_actions[action_count].key, sizeof(parsed_actions[action_count].key),
                        "%s", key ? key : "");
                snprintf(parsed_actions[action_count].label,
                        sizeof(parsed_actions[action_count].label), "%s",
                        (label && label[0]) ? label : "Open");
                action_count++;
            }
        }
        sd_bus_message_exit_container(msg);
    }

    /* hints: a{sv} — urgency, resident, image-path, image-data. */
    dc_urgency urgency = DC_URGENCY_NORMAL;
    const char *hint_image_path = NULL;
    bool resident = false;
    int32_t img_w = 0, img_h = 0, img_stride = 0, img_bps = 0, img_channels = 0;
    bool img_has_alpha = false;
    const void *img_data = NULL;
    size_t img_data_len = 0;

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
            } else if (key && strcmp(key, "resident") == 0) {
                int b = 0;
                sd_bus_message_enter_container(msg, 'v', "b");
                sd_bus_message_read_basic(msg, 'b', &b);
                sd_bus_message_exit_container(msg);
                resident = b != 0;
            } else if (key && (strcmp(key, "image-path") == 0 ||
                               strcmp(key, "image_path") == 0)) {
                sd_bus_message_enter_container(msg, 'v', "s");
                sd_bus_message_read_basic(msg, 's', &hint_image_path);
                sd_bus_message_exit_container(msg);
            } else if (key && (strcmp(key, "image-data") == 0 || strcmp(key, "image_data") == 0 ||
                               strcmp(key, "icon_data") == 0)) {
                /* (iiibiiay): width,height,rowstride,has_alpha,bps,channels,data.
                 * Falls through to the generic sd_bus_message_skip(msg, "v")
                 * below if the sender's actual variant contents don't match
                 * this signature (some senders' "icon_data" predates the
                 * spec settling on this exact layout). */
                if (sd_bus_message_enter_container(msg, 'v', "(iiibiiay)") >= 0) {
                    if (sd_bus_message_enter_container(msg, 'r', "iiibiiay") >= 0) {
                        int b = 0;
                        sd_bus_message_read(msg, "iii", &img_w, &img_h, &img_stride);
                        sd_bus_message_read_basic(msg, 'b', &b);
                        img_has_alpha = b != 0;
                        sd_bus_message_read(msg, "ii", &img_bps, &img_channels);
                        sd_bus_message_read_array(msg, 'y', &img_data, &img_data_len);
                        sd_bus_message_exit_container(msg); /* r */
                    }
                    sd_bus_message_exit_container(msg); /* v */
                } else {
                    sd_bus_message_skip(msg, "v");
                }
            } else {
                sd_bus_message_skip(msg, "v");
            }
            sd_bus_message_exit_container(msg); /* sv */
        }
        sd_bus_message_exit_container(msg); /* a{sv} */
    }

    sd_bus_message_read(msg, "i", &expire_timeout);

    unsigned char *decoded_pixels = NULL;
    int decoded_w = 0, decoded_h = 0;
    if (img_data && img_w > 0 && img_h > 0)
        decode_image_data(img_w, img_h, img_stride, img_has_alpha, img_bps, img_channels, img_data,
                          img_data_len, &decoded_pixels, &decoded_w, &decoded_h);

    uint32_t id = replaces_id != 0 ? replaces_id : ++n->next_id;
    dc_notification *slot = acquire_slot(n, replaces_id);
    if (slot->active && slot->id != id) {
        emit_closed(n, slot->id, DC_NOTIF_REASON_CLOSED); /* evicted a different notification */
    }
    free_slot_image(slot); /* release a reused slot's old pixels before memset drops the pointer */

    memset(slot, 0, sizeof(*slot));
    slot->id = id;
    slot->urgency = urgency;
    slot->expire_timeout_ms = expire_timeout; /* -1 default, 0 never, >0 ms */
    slot->created_ms = now_ms();
    slot->created_wall_ms = now_wall_ms();
    /* Do Not Disturb (Notifications tab): still recorded/history-tracked, just
     * never shown as a transient toast. */
    slot->popup = !dc_config_current->dnd_enabled;
    slot->active = true;
    slot->status = DC_NOTIF_CURRENT; /* arrival -> Current, even for a replace */
    slot->resident = resident;
    snprintf(slot->app_name, sizeof(slot->app_name), "%s", app_name ? app_name : "");
    snprintf(slot->summary, sizeof(slot->summary), "%s", summary ? summary : "");
    snprintf(slot->body, sizeof(slot->body), "%s", body ? body : "");
    snprintf(slot->app_icon, sizeof(slot->app_icon), "%s", app_icon ? app_icon : "");
    snprintf(slot->image_path, sizeof(slot->image_path), "%s", hint_image_path ? hint_image_path : "");
    slot->action_count = action_count;
    for (int i = 0; i < action_count; i++)
        slot->actions[i] = parsed_actions[i];
    slot->image_pixels = decoded_pixels;
    slot->image_w = decoded_w;
    slot->image_h = decoded_h;
    slot->image_version = decoded_pixels ? ++n->next_image_ver : 0;

    dc_info("notify #%u [%s] %s (%d action%s%s)", id, slot->app_name, slot->summary, action_count,
            action_count == 1 ? "" : "s", decoded_pixels ? ", image" : "");
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
        if (slot->status == DC_NOTIF_CURRENT) {
            slot->status = DC_NOTIF_HISTORY;
            slot->popup = false;
        }
        emit_closed(n, id, DC_NOTIF_REASON_CLOSED);
        notify_changed(n);
    }
    return sd_bus_reply_method_return(msg, "");
}

static int method_get_capabilities(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    (void)userdata;
    (void)err;
    return sd_bus_reply_method_return(msg, "as", 4, "body", "body-markup", "persistence", "actions");
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
        /* Another daemon (e.g. a user's already-running DMS) owns the
         * well-known name, so real notify-send traffic won't reach us --
         * but keep the object alive rather than failing outright: the
         * notification center still has a working local store (useful for
         * dc_notifications_seed_demo() when verifying it without a second
         * notification daemon to fight over the name). */
        dc_warn("could not own %s (another daemon running?): %s -- notification center will only "
                "show entries added locally", DC_NOTIF_NAME, strerror(-r));
    } else {
        dc_info("notification server registered (%s)", DC_NOTIF_NAME);
    }
    return n;
}

void dc_notifications_destroy(dc_notifications *n)
{
    if (!n)
        return;
    for (int i = 0; i < DC_NOTIF_MAX; i++)
        free_slot_image(&n->store[i]); /* plain malloc'd pixels -- no GL context needed to free these */
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

/* Milliseconds a toast should remain on-screen given its urgency/timeout.
 * Per-urgency defaults (server default, expire_timeout=-1) are settings-UI
 * configurable (Notifications tab -> notif_timeout_*_sec; 0 = never
 * auto-expire), matching DMS's notificationTimeoutLow/Normal/Critical. */
static int lifetime_ms(const dc_notification *item)
{
    if (item->expire_timeout_ms > 0)
        return item->expire_timeout_ms;
    if (item->expire_timeout_ms == 0)
        return 0; /* never auto-expire */
    /* server default (-1) */
    const dc_config *cfg = dc_config_current;
    if (item->urgency == DC_URGENCY_CRITICAL)
        return cfg->notif_timeout_critical_sec * 1000;
    if (item->urgency == DC_URGENCY_LOW)
        return cfg->notif_timeout_low_sec * 1000;
    return cfg->notif_timeout_normal_sec * 1000;
}

bool dc_notifications_tick(dc_notifications *n)
{
    if (!n)
        return false;
    bool changed = false;
    int64_t t = now_ms();
    for (int i = 0; i < DC_NOTIF_MAX; i++) {
        dc_notification *item = &n->store[i];
        if (!item->active || !item->popup)
            continue;
        int life = lifetime_ms(item);
        if (life > 0 && t - item->created_ms >= life) {
            /* Only hides the toast -- stays on the Current tab until the user
             * dismisses it (matches DMS: a popup timeout doesn't drop the
             * notification from NotificationService.notifications). */
            item->popup = false;
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
            dc_notification *item = &n->store[i];
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

/* Shared newest-first selection scan for a status-filtered list (Current or
 * History) -- same O(n*max) selection-sort style as dc_notifications_popups()
 * above; store is small (DC_NOTIF_MAX=64) so this stays cheap. */
static int select_by_status(dc_notifications *n, dc_notif_status status, const dc_notification **out,
                            int max)
{
    if (!n || max <= 0)
        return 0;
    int count = 0;
    int64_t last = INT64_MAX;
    for (int picked = 0; picked < max; picked++) {
        dc_notification *best = NULL;
        for (int i = 0; i < DC_NOTIF_MAX; i++) {
            dc_notification *item = &n->store[i];
            if (!item->active || item->status != status)
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

int dc_notifications_current(dc_notifications *n, const dc_notification **out, int max)
{
    return select_by_status(n, DC_NOTIF_CURRENT, out, max);
}

int dc_notifications_current_count(dc_notifications *n)
{
    if (!n)
        return 0;
    int count = 0;
    for (int i = 0; i < DC_NOTIF_MAX; i++)
        if (n->store[i].active && n->store[i].status == DC_NOTIF_CURRENT)
            count++;
    return count;
}

int dc_notifications_history(dc_notifications *n, const dc_notification **out, int max)
{
    return select_by_status(n, DC_NOTIF_HISTORY, out, max);
}

int dc_notifications_history_count(dc_notifications *n)
{
    if (!n)
        return 0;
    int count = 0;
    for (int i = 0; i < DC_NOTIF_MAX; i++)
        if (n->store[i].active && n->store[i].status == DC_NOTIF_HISTORY)
            count++;
    return count;
}

void dc_notifications_clear_current(dc_notifications *n)
{
    if (!n)
        return;
    for (int i = 0; i < DC_NOTIF_MAX; i++) {
        dc_notification *s = &n->store[i];
        if (s->active && s->status == DC_NOTIF_CURRENT) {
            s->status = DC_NOTIF_HISTORY;
            s->popup = false;
            emit_closed(n, s->id, DC_NOTIF_REASON_DISMISSED);
        }
    }
    notify_changed(n);
}

void dc_notifications_clear_history(dc_notifications *n)
{
    if (!n)
        return;
    for (int i = 0; i < DC_NOTIF_MAX; i++) {
        dc_notification *s = &n->store[i];
        if (s->active && s->status == DC_NOTIF_HISTORY) {
            s->active = false;
            free_slot_image(s);
        }
    }
    notify_changed(n);
}

void dc_notifications_dismiss(dc_notifications *n, uint32_t id)
{
    if (!n)
        return;
    dc_notification *slot = find_by_id(n, id);
    if (!slot)
        return;
    resolve_dismiss(n, slot);
}

void dc_notifications_invoke_action(dc_notifications *n, uint32_t id, int action_index)
{
    if (!n)
        return;
    dc_notification *slot = find_by_id(n, id);
    if (!slot || action_index < 0 || action_index >= slot->action_count)
        return;
    sd_bus_emit_signal(n->bus, DC_NOTIF_PATH, DC_NOTIF_IFACE, "ActionInvoked", "us", id,
                       slot->actions[action_index].key);
    if (slot->resident)
        return; /* spec: a resident notification isn't auto-removed after an action */
    resolve_dismiss(n, slot);
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

/* Fabricated entries covering: a plain body, a long/wrapping body, an action
 * button, and both same-day and older (different calendar day) timestamps --
 * see docs/13-POPOUTS-SPEC.md sec.3. Bypasses D-Bus entirely (writes directly
 * into the store) since a user's already-running DMS instance normally owns
 * org.freedesktop.Notifications, so real notify-send traffic never reaches
 * DankC's server for manual verification. */
void dc_notifications_seed_demo(dc_notifications *n)
{
    if (!n)
        return;

    struct {
        const char *app, *summary, *body;
        const char *action_key[2], *action_label[2]; /* up to 2 demo actions; "" = unused slot */
        int64_t age_ms;
        dc_notif_status status;
    } demo[] = {
        {"notify-send", "DankC test", "Hello from notify-send", {"", ""}, {"", ""}, 5 * 60 * 1000,
         DC_NOTIF_CURRENT},
        {"DMS", "Screenshot captured", "Copied to clipboard\nscreenshot-2026-07-01-27-26.png",
         {"open", ""}, {"Open", ""}, (int64_t)27 * 3600 * 1000, DC_NOTIF_CURRENT},
        {"NetworkManager Applet", "Connection Established",
         "You are now connected to the Wi-Fi network \"BAIHQ\".", {"connect", "dont-show"},
         {"Reconnect", "Don't show this message again"}, (int64_t)32 * 3600 * 1000,
         DC_NOTIF_CURRENT},
        {"Spotify", "Now Playing", "Currently Playing \xe2\x80\x94 Demo Artist", {"", ""}, {"", ""},
         2 * 3600 * 1000, DC_NOTIF_HISTORY},
        {"Firefox", "Download complete", "installer.AppImage finished downloading (128 MB)",
         {"", ""}, {"", ""}, (int64_t)26 * 3600 * 1000, DC_NOTIF_HISTORY},
        {"Slack", "New message from Alice",
         "Hey, are we still meeting today at 3pm to discuss the quarterly roadmap and budget "
         "planning for next sprint cycle? Let me know if that still works for you or if we need "
         "to reschedule.",
         {"reply", "mark-read"}, {"Reply", "Mark as read"}, 3 * 3600 * 1000, DC_NOTIF_HISTORY},
        {"systemd", "Update available", "A new system update is ready to install.", {"", ""},
         {"", ""}, (int64_t)3 * 24 * 3600 * 1000, DC_NOTIF_HISTORY},
        {"Mail", "Inbox (3)", "You have 3 unread messages.", {"open", ""}, {"Open", ""},
         5 * 3600 * 1000, DC_NOTIF_HISTORY},
    };

    int64_t mono = now_ms();
    int64_t wall = now_wall_ms();

    for (size_t i = 0; i < sizeof(demo) / sizeof(demo[0]); i++) {
        dc_notification *slot = NULL;
        for (int j = 0; j < DC_NOTIF_MAX; j++) {
            if (!n->store[j].active) {
                slot = &n->store[j];
                break;
            }
        }
        if (!slot)
            break;

        memset(slot, 0, sizeof(*slot));
        slot->id = ++n->next_id;
        slot->urgency = DC_URGENCY_NORMAL;
        slot->expire_timeout_ms = 0;
        slot->created_ms = mono - demo[i].age_ms;
        slot->created_wall_ms = wall - demo[i].age_ms;
        slot->popup = false; /* demo entries are pre-seeded, not live toasts */
        slot->active = true;
        slot->status = demo[i].status;
        snprintf(slot->app_name, sizeof(slot->app_name), "%s", demo[i].app);
        snprintf(slot->summary, sizeof(slot->summary), "%s", demo[i].summary);
        snprintf(slot->body, sizeof(slot->body), "%s", demo[i].body);
        for (int a = 0; a < 2; a++) {
            if (!demo[i].action_key[a][0])
                continue;
            dc_notif_action *act = &slot->actions[slot->action_count++];
            snprintf(act->key, sizeof(act->key), "%s", demo[i].action_key[a]);
            snprintf(act->label, sizeof(act->label), "%s", demo[i].action_label[a]);
        }
    }

    n->has_unread = true;
    notify_changed(n);
}
