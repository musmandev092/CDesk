#include "services/dbus.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"

#include <poll.h>
#include <stdlib.h>

static void bus_drain(sd_bus *bus)
{
    for (;;) {
        int r = sd_bus_process(bus, NULL);
        if (r <= 0)
            break;
    }
}

/* Poll woke us: read + dispatch any pending messages. */
static void bus_fd_ready(int fd, uint32_t revents, void *data)
{
    DC_UNUSED(fd);
    DC_UNUSED(revents);
    bus_drain((sd_bus *)data);
}

/* Before blocking: drain anything already buffered and flush outgoing. */
static void bus_prepare(void *data)
{
    sd_bus *bus = data;
    bus_drain(bus);
    sd_bus_flush(bus);
}

dc_dbus *dc_dbus_connect(void)
{
    dc_dbus *bus = calloc(1, sizeof(*bus));

    if (sd_bus_open_system(&bus->system) < 0) {
        dc_warn("sd_bus_open_system failed; system-bus services disabled");
        bus->system = NULL;
    }
    if (sd_bus_open_user(&bus->user) < 0) {
        dc_warn("sd_bus_open_user failed; session-bus services disabled");
        bus->user = NULL;
    }
    if (!bus->system && !bus->user) {
        free(bus);
        return NULL;
    }

    dc_info("sd-bus connected (system=%s, user=%s)", bus->system ? "ok" : "no",
            bus->user ? "ok" : "no");
    return bus;
}

void dc_dbus_destroy(dc_dbus *bus)
{
    if (!bus)
        return;
    if (bus->system)
        sd_bus_flush_close_unref(bus->system);
    if (bus->user)
        sd_bus_flush_close_unref(bus->user);
    free(bus);
}

void dc_dbus_integrate(dc_dbus *bus, struct dc_loop *loop)
{
    if (!bus)
        return;
    if (bus->system) {
        dc_loop_add_fd(loop, sd_bus_get_fd(bus->system), POLLIN, bus_fd_ready, bus->system);
        dc_loop_add_prepare(loop, bus_prepare, bus->system);
    }
    if (bus->user) {
        dc_loop_add_fd(loop, sd_bus_get_fd(bus->user), POLLIN, bus_fd_ready, bus->user);
        dc_loop_add_prepare(loop, bus_prepare, bus->user);
    }
}
