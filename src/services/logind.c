#include "services/logind.h"

#include "core/log.h"
#include "dc.h"
#include "services/dbus.h"

#include <stdlib.h>

#define DC_LOGIND_DEST "org.freedesktop.login1"
#define DC_LOGIND_MANAGER_IFACE "org.freedesktop.login1.Manager"
#define DC_LOGIND_SESSION_IFACE "org.freedesktop.login1.Session"

struct dc_logind {
    sd_bus *bus;
    sd_bus_slot *sleep_slot;
    sd_bus_slot *lock_slot;
    dc_logind_lock_cb cb;
    void *cb_data;
};

/* PrepareForSleep(b): true just before the system suspends -> lock. */
static int on_prepare_for_sleep(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    DC_UNUSED(err);
    dc_logind *l = userdata;
    int start = 0;
    if (sd_bus_message_read(msg, "b", &start) < 0)
        return 0;
    if (start && l->cb) {
        dc_info("logind: locking before sleep");
        l->cb(l->cb_data);
    }
    return 0;
}

/* Session.Lock: an explicit lock request (e.g. loginctl lock-session). */
static int on_session_lock(sd_bus_message *msg, void *userdata, sd_bus_error *err)
{
    DC_UNUSED(msg);
    DC_UNUSED(err);
    dc_logind *l = userdata;
    if (l->cb) {
        dc_info("logind: lock requested");
        l->cb(l->cb_data);
    }
    return 0;
}

dc_logind *dc_logind_create(struct dc_dbus *dbus, dc_logind_lock_cb cb, void *user_data)
{
    if (!dbus || !dbus->system)
        return NULL;

    dc_logind *l = calloc(1, sizeof(*l));
    l->bus = dbus->system;
    l->cb = cb;
    l->cb_data = user_data;

    sd_bus_match_signal(l->bus, &l->sleep_slot, DC_LOGIND_DEST, NULL, DC_LOGIND_MANAGER_IFACE,
                        "PrepareForSleep", on_prepare_for_sleep, l);
    sd_bus_match_signal(l->bus, &l->lock_slot, DC_LOGIND_DEST, NULL, DC_LOGIND_SESSION_IFACE, "Lock",
                        on_session_lock, l);

    dc_info("logind: watching sleep + lock signals");
    return l;
}

void dc_logind_destroy(dc_logind *l)
{
    if (!l)
        return;
    sd_bus_slot_unref(l->sleep_slot);
    sd_bus_slot_unref(l->lock_slot);
    free(l);
}
