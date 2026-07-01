/* main.c — DankC entry point.
 *
 * Milestone 2: connect to niri via Wayland, bring up EGL + nanovg, and place a
 * themed bar with a live clock on every output. Later milestones add services,
 * more widgets, and the rest of the shell (see docs/06-ROADMAP.md).
 */
#include "core/log.h"
#include "core/loop.h"
#include "dc.h"
#include "niri/niri.h"
#include "render/nvg.h"
#include "services/bluez.h"
#include "services/dbus.h"
#include "services/mpris.h"
#include "theme/theme.h"
#include "ui/bar/bar.h"
#include "ui/controlcenter.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/timerfd.h>
#include <unistd.h>

#define DC_MAX_BARS 16

static dc_loop *g_loop = NULL;

struct bar_set {
    dc_bar *bars[DC_MAX_BARS];
    int count;
};

static void handle_signal(int signum)
{
    DC_UNUSED(signum);
    if (g_loop)
        dc_loop_stop(g_loop);
}

static void render_all(struct bar_set *set)
{
    for (int i = 0; i < set->count; i++)
        dc_bar_render(set->bars[i]);
}

/* Fires once per second; redraws every bar so the clock stays current. */
static void clock_tick(int fd, uint32_t revents, void *data)
{
    DC_UNUSED(revents);
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) < 0)
        return;
    render_all(data);
}

/* Called when niri's workspace state changes. */
static void niri_changed(void *data)
{
    render_all(data);
}

struct click_ctx {
    struct bar_set *set;
    dc_control_center *control_center;
};

/* Route a left click to the bar under the pointer and act on the region. */
static void handle_bar_click(struct wl_surface *surface, double x, double y, void *data)
{
    struct click_ctx *ctx = data;
    for (int i = 0; i < ctx->set->count; i++) {
        dc_bar *bar = ctx->set->bars[i];
        if (dc_bar_surface(bar) != surface)
            continue;
        dc_bar_region region = dc_bar_hittest(bar, x, y);
        if (region == DC_BAR_REGION_CONTROL_CENTER)
            dc_control_center_toggle(ctx->control_center, dc_bar_output(bar));
        return;
    }
}

static int create_clock_timer(void)
{
    int fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (fd < 0)
        return -1;
    struct itimerspec spec = {
        .it_interval = {.tv_sec = 1, .tv_nsec = 0},
        .it_value = {.tv_sec = 1, .tv_nsec = 0},
    };
    timerfd_settime(fd, 0, &spec, NULL);
    return fd;
}

int main(void)
{
    dc_log_init(DC_LOG_DEBUG);
    dc_info("DankC %s starting", DC_VERSION);
    dc_theme_init();

    dc_wayland *wl = dc_wayland_connect();
    if (!wl)
        return 1;

    dc_egl egl = {0};
    if (!dc_egl_init(&egl, wl->display)) {
        dc_wayland_destroy(wl);
        return 1;
    }

    dc_niri *niri = dc_niri_connect();
    dc_dbus *dbus = dc_dbus_connect();
    dc_bluez_init(dbus);
    dc_mpris_init(dbus);

    dc_render render = {0};
    struct bar_set set = {0};
    dc_output *output;
    wl_list_for_each(output, &wl->outputs, link) {
        if (set.count >= DC_MAX_BARS) {
            dc_warn("more than %d outputs; ignoring the rest", DC_MAX_BARS);
            break;
        }
        set.bars[set.count++] = dc_bar_create(wl, output, &egl, &render, niri);
    }
    if (set.count == 0)
        dc_warn("no outputs found; nothing to display");

    dc_control_center *control_center = dc_control_center_create(wl, &egl, &render);
    struct click_ctx cctx = {.set = &set, .control_center = control_center};

    g_loop = dc_loop_create();
    dc_wayland_integrate(wl, g_loop);
    dc_niri_integrate(niri, g_loop);
    dc_niri_set_changed_cb(niri, niri_changed, &set);
    dc_wayland_set_click_cb(wl, handle_bar_click, &cctx);
    dc_dbus_integrate(dbus, g_loop);

    int clock_fd = create_clock_timer();
    if (clock_fd >= 0)
        dc_loop_add_fd(g_loop, clock_fd, POLLIN, clock_tick, &set);

    struct sigaction sa = {.sa_handler = handle_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    /* TEMP(verify): auto-open the control center to screenshot it. */
    if (getenv("DANKC_CC_DEMO")) {
        dc_output *first = NULL;
        wl_list_for_each(first, &wl->outputs, link) {
            break;
        }
        if (first)
            dc_control_center_toggle(control_center, first);
    }

    dc_info("entering event loop (%d bar%s)", set.count, set.count == 1 ? "" : "s");
    dc_loop_run(g_loop);

    dc_info("shutting down");
    if (clock_fd >= 0)
        close(clock_fd);
    dc_control_center_destroy(control_center);
    for (int i = 0; i < set.count; i++)
        dc_bar_destroy(set.bars[i]);
    /* GL teardown is skipped: the process is exiting and nvgDelete needs a live
     * context; the driver reclaims resources on exit. */
    dc_loop_destroy(g_loop);
    dc_dbus_destroy(dbus);
    dc_niri_destroy(niri);
    dc_egl_finish(&egl);
    dc_wayland_destroy(wl);
    return 0;
}
