/* logind.h — react to systemd-logind session events.
 *
 * Locks the session before sleep (PrepareForSleep) and on a lock request
 * (loginctl lock-session / Session.Lock). See docs/03-SERVICES. Feeds the lock
 * screen (T22).
 */
#ifndef DC_SERVICES_LOGIND_H
#define DC_SERVICES_LOGIND_H

struct dc_dbus;

typedef struct dc_logind dc_logind;

/* Called when logind asks us to lock (pre-sleep or explicit lock). */
typedef void (*dc_logind_lock_cb)(void *user_data);

/* Subscribe on the system bus. NULL if unavailable. */
dc_logind *dc_logind_create(struct dc_dbus *dbus, dc_logind_lock_cb cb, void *user_data);
void dc_logind_destroy(dc_logind *l);

#endif /* DC_SERVICES_LOGIND_H */
