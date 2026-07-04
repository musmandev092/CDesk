/* greeter_main.c — `dankc greeter` subcommand: reduced-init entry point
 * (docs/28-GREETER-PLAN.md T4).
 *
 * greetd execs `dankc greeter` directly on the greeter's VT -- there is no
 * niri session, no D-Bus session/system-bus-dependent shell state, and no
 * user logged in yet. So this is deliberately NOT main()'s normal startup
 * path (see main.c): it brings up only what the greeter UI (ui/greeter.c)
 * actually needs -- logging, theme, config (for fonts/colors/scale
 * preferences), the Wayland connection, EGL, the render context, and the
 * event loop -- and skips everything else main() starts (D-Bus, tray,
 * notifications, niri IPC, polkit, autostart, night light, the control
 * socket, the bar/dock/panels/popouts). Those all either need a running
 * niri+D-Bus session (which doesn't exist pre-login) or are simply
 * irrelevant to a login screen.
 *
 * Process lifecycle: greetd's protocol (services/greetd.h) waits for THIS
 * PROCESS TO EXIT after a successful start_session before it hands the seat
 * to the newly-started session -- so once dc_greeter_create()'s done_cb
 * fires (greeter.c: STARTING -> DONE), the only correct thing to do is tear
 * down and exit as promptly as possible. We use _exit(0) rather than a
 * normal return, skipping libc atexit/stdio-flush machinery that has nothing
 * useful to do here anyway (see run() below).
 */
#include "ui/greeter_main.h"

#include "core/config.h"
#include "core/log.h"
#include "core/loop.h"
#include "dc.h"
#include "render/nvg.h"
#include "theme/theme.h"
#include "ui/greeter.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static dc_loop *g_greeter_loop = NULL;

static void greeter_signal(int signum)
{
    DC_UNUSED(signum);
    if (g_greeter_loop)
        dc_loop_stop(g_greeter_loop);
}

/* dc_wayland's key-repeat timerfd -- same plumbing as main.c's
 * wl_repeat_readable(), duplicated here rather than shared because main.c
 * isn't a header the greeter should depend on. */
static void greeter_repeat_readable(int fd, uint32_t revents, void *user_data)
{
    DC_UNUSED(fd);
    DC_UNUSED(revents);
    dc_wayland_repeat_fire((dc_wayland *)user_data);
}

/* Adapt dc_wayland's (keysym, utf8, user_data) key callback onto
 * dc_greeter_handle_key's (g, keysym, utf8) — same values, different
 * argument order, so a direct function-pointer cast isn't an option. */
static void greeter_key_cb(uint32_t keysym, const char *utf8, void *user_data)
{
    dc_greeter_handle_key((dc_greeter *)user_data, keysym, utf8);
}

/* Adapt dc_wayland's click callback: the greeter only cares about left
 * clicks (dc_greeter_handle_click's own doc comment describes it as
 * "left-click"), matching every other left-click-only surface in main.c
 * (e.g. the launcher, settings, popouts). */
static void greeter_click_cb(struct wl_surface *surface, double x, double y, uint32_t button,
                             void *user_data)
{
    if (button != BTN_LEFT)
        return;
    dc_greeter_handle_click((dc_greeter *)user_data, surface, x, y);
}

static void greeter_motion_cb(struct wl_surface *surface, double x, double y, void *user_data)
{
    dc_greeter_handle_motion((dc_greeter *)user_data, surface, x, y);
}

/* dc_greeter_create()'s done_cb: fired exactly once, after greetd confirms
 * the chosen session actually launched. Just stop the loop -- run() below
 * does the actual teardown + _exit(0) once dc_loop_run() returns. */
static void greeter_done_cb(void *user_data)
{
    dc_loop_stop((dc_loop *)user_data);
}

static int run(void)
{
    dc_theme_init();
    dc_config_load();

    dc_wayland *wl = dc_wayland_connect();
    if (!wl) {
        dc_error("greeter: could not connect to Wayland display");
        return 1;
    }

    dc_egl egl = {0};
    if (!dc_egl_init(&egl, wl->display)) {
        dc_error("greeter: EGL init failed");
        dc_wayland_destroy(wl);
        return 1;
    }

    dc_render render = {0}; /* lazily realised by dc_render_ensure() (greeter.c) */

    dc_loop *loop = dc_loop_create();
    g_greeter_loop = loop;
    signal(SIGINT, greeter_signal);
    signal(SIGTERM, greeter_signal);

    dc_wayland_integrate(wl, loop);
    if (dc_wayland_repeat_fd(wl) >= 0)
        dc_loop_add_fd(loop, dc_wayland_repeat_fd(wl), POLLIN, greeter_repeat_readable, wl);

    dc_greeter *greeter = dc_greeter_create(wl, &egl, &render, loop, greeter_done_cb, loop);

    dc_wayland_set_key_cb(wl, greeter_key_cb, greeter);
    dc_wayland_set_click_cb(wl, greeter_click_cb, greeter);
    dc_wayland_set_motion_cb(wl, greeter_motion_cb, greeter);

    dc_info("greeter: entering event loop");
    dc_loop_run(loop);

    /* Reached either because the greeter finished (session started -- exit
     * promptly, see this file's top comment) or a signal asked us to stop
     * (interactive testing, e.g. Ctrl+C in the demo). Either way the
     * teardown is the same; only the exit mechanism differs on purpose. */
    dc_greeter_destroy(greeter);
    dc_egl_finish(&egl);
    dc_wayland_destroy(wl);
    dc_loop_destroy(loop);
    _exit(0);
}

int dc_greeter_main(int argc, char **argv)
{
    DC_UNUSED(argc);
    DC_UNUSED(argv);

    dc_log_init(DC_LOG_DEBUG);
    dc_info("DankC %s starting (greeter mode)", DC_VERSION);

    /* greetd hands `dankc greeter` its own socket via $GREETD_SOCK (or, for
     * a mock server used in testing, $DANKC_GREETD_SOCK_PATH -- see
     * services/greetd.h's dc_greetd_create()). Outside of one of those two,
     * running for real would just sit there unable to authenticate anyone,
     * so refuse -- UNLESS $DANKC_GREETER_DEMO=1, which exists precisely to
     * preview the UI on an ordinary desktop session (no greetd anywhere in
     * sight). In demo mode we still call dc_greeter_create() completely
     * normally; it already degrades gracefully with no $GREETD_SOCK set
     * (dc_greetd_create() returns NULL, greeter.c shows a "greetd
     * unavailable" status line instead of crashing) -- that existing
     * fallback IS the demo's fake/inert prompt state, so nothing here needs
     * to duplicate or fake greetd itself. */
    bool demo = getenv("DANKC_GREETER_DEMO") && strcmp(getenv("DANKC_GREETER_DEMO"), "1") == 0;
    bool have_sock = getenv("GREETD_SOCK") || getenv("DANKC_GREETD_SOCK_PATH");
    if (!demo && !have_sock) {
        dc_error("greeter: $GREETD_SOCK is not set -- refusing to run outside greetd "
                "(set DANKC_GREETER_DEMO=1 to preview the UI without a real greetd)");
        return 1;
    }
    if (demo)
        dc_info("greeter: DANKC_GREETER_DEMO=1 -- running without a real greetd connection");

    return run();
}
