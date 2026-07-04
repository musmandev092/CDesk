/* notifications.h — org.freedesktop.Notifications server (session bus).
 *
 * Implements the Desktop Notifications spec so apps can post notifications to
 * DankC. Stores active notifications and drives the toast popups + the
 * notification center, matching DMS's NotificationService. See docs/03-SERVICES.
 */
#ifndef DC_SERVICES_NOTIFICATIONS_H
#define DC_SERVICES_NOTIFICATIONS_H

#include <stdbool.h>
#include <stdint.h>

struct dc_dbus;

/* Single store for both the Notification Center's "Current" and "History"
 * tabs (docs/13-POPOUTS-SPEC.md sec.3) -- see dc_notif_status below. Was two
 * separate arrays (32 live + 64 history-ring) before the tabs split; bumped to
 * one flat 64-entry store sized like the old history ring since every
 * notification now lives here for its whole life. */
#define DC_NOTIF_MAX 64
#define DC_NOTIF_APP 64
#define DC_NOTIF_SUMMARY 160
#define DC_NOTIF_BODY 400
#define DC_NOTIF_ICON 160
#define DC_NOTIF_ACTION 48

/* Action-buttons row (docs/13-POPOUTS-SPEC.md sec.3; DMS's NotificationPopup/
 * NotificationCard render a Repeater over the "default"-filtered actions
 * array). Capped at 4: DMS's own layout hides its separate "Dismiss" pill
 * once actionCount>=3, i.e. it isn't designed for more than a handful of
 * buttons either; 4 is generous headroom over what any real sender sends
 * (typically 1-2) while keeping dc_notification's fixed size bounded. */
#define DC_NOTIF_ACTION_MAX 4

/* Inline image bound (image-data hint, iiibiiay): stored pixels are
 * downscaled (nearest-neighbor) to fit within this many px on the long side
 * before being kept, so a single notification can never pin down more than
 * DC_NOTIF_IMAGE_MAX_DIM^2*4 bytes -- see notifications.c's
 * decode_image_data(). 128 comfortably covers every on-screen use (toast/
 * center avatar circle is ~40px) while being sharp on HiDPI. */
#define DC_NOTIF_IMAGE_MAX_DIM 128

/* Memory bound for History: only the N most-recently-archived History
 * entries keep their decoded image-data pixels; older ones have
 * image_pixels freed (falls back to re-resolving image_path/app_icon from
 * disk, which is just a path string -- effectively free to keep forever).
 * See notifications.c's enforce_history_image_cap(). */
#define DC_NOTIF_HISTORY_IMAGE_KEEP 8

typedef enum {
    DC_URGENCY_LOW = 0,
    DC_URGENCY_NORMAL = 1,
    DC_URGENCY_CRITICAL = 2,
} dc_urgency;

/* Notification Center tab membership. DMS keeps every notification in a
 * persistent `historyList` from the moment it arrives while *also* tracking
 * still-open ones in a separate `notifications` array (so "current" is a
 * near-subset of "history" that shrinks as toasts get retained/dropped).
 * DankC uses a simpler, mutually-exclusive split that's easier to reason
 * about from a single flat store: a notification starts CURRENT on arrival
 * and moves to HISTORY only when the user acts on it -- a card's X/Dismiss
 * (or the "Open" action button, which also resolves the card), or "Clear" on
 * the Current tab (moves everything at once). "Clear" on the History tab (or
 * dismissing/opening an already-HISTORY card) deletes instead of moving,
 * since there's nowhere further for it to go. See notifications.c's
 * resolve_dismiss(). */
typedef enum {
    DC_NOTIF_CURRENT = 0,
    DC_NOTIF_HISTORY = 1,
} dc_notif_status;

/* One action button: key is what's sent back in ActionInvoked, label is the
 * button text. The spec's reserved "default" key (invoked by clicking the
 * notification body itself, not a button) is filtered out at parse time --
 * see method_notify() -- so everything in dc_notification.actions[] is a
 * real, clickable button. */
typedef struct {
    char key[DC_NOTIF_ACTION];
    char label[DC_NOTIF_ACTION];
} dc_notif_action;

typedef struct {
    uint32_t id;
    char app_name[DC_NOTIF_APP];
    char summary[DC_NOTIF_SUMMARY];
    char body[DC_NOTIF_BODY];
    char app_icon[DC_NOTIF_ICON]; /* icon name (XDG theme) or absolute path from the Notify() app_icon arg / icon_data hint */
    char image_path[DC_NOTIF_ICON]; /* image-path hint: absolute file path, higher priority than app_icon */
    dc_notif_action actions[DC_NOTIF_ACTION_MAX];
    int action_count;
    bool resident; /* "resident" hint: don't auto-dismiss the card after ActionInvoked */

    /* Decoded image-data hint (iiibiiay), downscaled to <=DC_NOTIF_IMAGE_MAX_DIM
     * per side -- RGBA8, row-major, malloc'd by decode_image_data(); NULL if
     * the app didn't send this hint (image_path/app_icon are the fallback,
     * resolved+cached by the renderer instead since they're just paths).
     * Freed on slot reuse/clear/destroy and by enforce_history_image_cap()
     * once archived past DC_NOTIF_HISTORY_IMAGE_KEEP -- see notifications.c.
     * image_version bumps every time this notification's slot is (re)filled
     * by Notify() so a renderer's nvg-image cache (keyed by id+version) knows
     * to reload after a replaces_id update, even though `id` itself is
     * unchanged. */
    unsigned char *image_pixels;
    int image_w, image_h;
    uint32_t image_version;

    dc_urgency urgency;
    int expire_timeout_ms; /* -1 = server default, 0 = never expire */
    int64_t created_ms;    /* CLOCK_MONOTONIC ms when posted -- lifetime/ordering math */
    int64_t created_wall_ms; /* CLOCK_REALTIME ms when posted -- wall-clock display only */
    bool popup;            /* still shown as a transient toast */
    bool active;           /* occupies this slot (either tab; false = free/deleted) */
    dc_notif_status status;
} dc_notification;

typedef struct dc_notifications dc_notifications;

/* Called whenever the notification set changes (new toast, expiry, close). */
typedef void (*dc_notif_changed_cb)(void *user_data);

/* Register the server on the session bus. Returns NULL if `dbus`/user bus is
 * unavailable or the well-known name is already owned by another daemon. */
dc_notifications *dc_notifications_create(struct dc_dbus *dbus);
void dc_notifications_destroy(dc_notifications *n);

void dc_notifications_set_changed_cb(dc_notifications *n, dc_notif_changed_cb cb, void *user_data);

/* Expire any toasts whose timeout has elapsed (call from the 1 Hz tick). Only
 * hides the transient toast (popup=false); the entry stays on the Current tab
 * until the user acts on it, matching DMS (a toast auto-hiding doesn't drop it
 * from NotificationService.notifications). Returns true if anything changed. */
bool dc_notifications_tick(dc_notifications *n);

/* Enumerate the currently-visible toast popups, newest first, up to `max`.
 * Writes pointers into `out` and returns the count. */
int dc_notifications_popups(dc_notifications *n, const dc_notification **out, int max);

/* Enumerate the Current tab (arrived, not yet dismissed), newest first, up to
 * `max`. Writes pointers into `out` and returns the count. */
int dc_notifications_current(dc_notifications *n, const dc_notification **out, int max);
int dc_notifications_current_count(dc_notifications *n);

/* Enumerate the History tab (dismissed/cleared), newest first, up to `max`.
 * Writes pointers into `out` and returns the count. */
int dc_notifications_history(dc_notifications *n, const dc_notification **out, int max);
int dc_notifications_history_count(dc_notifications *n);

/* "Clear" on the Current tab: move every Current entry to History (does not
 * delete). */
void dc_notifications_clear_current(dc_notifications *n);

/* "Clear" on the History tab: delete every History entry. */
void dc_notifications_clear_history(dc_notifications *n);

/* Per-card X/Dismiss: resolves `id` one step further (Current -> History,
 * History -> deleted). Also used for a D-Bus/programmatic dismiss (e.g. a
 * toast's own close button). Emits NotificationClosed only on the
 * Current->History step, matching the spec (a History entry was already
 * closed once). */
void dc_notifications_dismiss(dc_notifications *n, uint32_t id);

/* Per-card action button: emits ActionInvoked for `id`'s actions[action_index],
 * then resolves it exactly like dc_notifications_dismiss() -- UNLESS the
 * notification carries the "resident" hint, in which case it stays put (spec:
 * a resident notification isn't auto-removed after an action). No-op if `id`
 * doesn't exist or action_index is out of range. */
void dc_notifications_invoke_action(dc_notifications *n, uint32_t id, int action_index);

/* True if a notification has arrived since the notification center was last
 * opened (docs/12-BAR-SPEC.md sec.4 notificationButton: the bell's unread
 * dot). Survives toast expiry/dismissal — only dc_notifications_mark_read()
 * clears it. */
bool dc_notifications_has_unread(dc_notifications *n);

/* Clear the unread flag (call when the notification center becomes visible). */
void dc_notifications_mark_read(dc_notifications *n);

/* Seed a handful of fixed demo entries (some Current, some History; varied
 * bodies/actions/ages) bypassing D-Bus entirely, for screenshotting/manual
 * verification when nothing owns org.freedesktop.Notifications for real (e.g.
 * a user's live DMS already does). Gated by the caller on $DANKC_NC_DEMO. */
void dc_notifications_seed_demo(dc_notifications *n);

/* Post a notification from internal dankc code (battery automation, etc.)
 * without going through D-Bus. Drives the same toast/sound/notification-
 * center effects a real Notify() arrival would, including the Do Not Disturb
 * gate, off the same id counter as D-Bus-originated notifications. Returns
 * the assigned id (0 if `n` is NULL). */
uint32_t dc_notifications_post_local(dc_notifications *n, const char *app, const char *summary,
                                     const char *body, dc_urgency urgency);

#endif /* DC_SERVICES_NOTIFICATIONS_H */
