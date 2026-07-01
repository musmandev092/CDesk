/* main.c — DankC entry point.
 *
 * Milestone 1: connect to niri via Wayland, bring up EGL, and place a themed
 * bar on every output. Later milestones add services, widgets, and the rest of
 * the shell (see docs/06-ROADMAP.md).
 */
#include "core/log.h"
#include "core/loop.h"
#include "dc.h"
#include "ui/bar/bar.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <signal.h>
#include <stdlib.h>

#define DC_MAX_BARS 16

static dc_loop *g_loop = NULL;

static void handle_signal(int signum)
{
    DC_UNUSED(signum);
    if (g_loop)
        dc_loop_stop(g_loop);
}

int main(void)
{
    dc_log_init(DC_LOG_DEBUG);
    dc_info("DankC %s starting", DC_VERSION);

    dc_wayland *wl = dc_wayland_connect();
    if (!wl)
        return 1;

    dc_egl egl = {0};
    if (!dc_egl_init(&egl, wl->display)) {
        dc_wayland_destroy(wl);
        return 1;
    }

    dc_bar *bars[DC_MAX_BARS];
    int bar_count = 0;
    dc_output *output;
    wl_list_for_each(output, &wl->outputs, link) {
        if (bar_count >= DC_MAX_BARS) {
            dc_warn("more than %d outputs; ignoring the rest", DC_MAX_BARS);
            break;
        }
        bars[bar_count++] = dc_bar_create(wl, output, &egl);
    }
    if (bar_count == 0)
        dc_warn("no outputs found; nothing to display");

    g_loop = dc_loop_create();
    dc_wayland_integrate(wl, g_loop);

    struct sigaction sa = {.sa_handler = handle_signal};
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    dc_info("entering event loop (%d bar%s)", bar_count, bar_count == 1 ? "" : "s");
    dc_loop_run(g_loop);

    dc_info("shutting down");
    for (int i = 0; i < bar_count; i++)
        dc_bar_destroy(bars[i]);
    dc_loop_destroy(g_loop);
    dc_egl_finish(&egl);
    dc_wayland_destroy(wl);
    return 0;
}
