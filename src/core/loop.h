/* loop.h — the single poll()-based event loop.
 *
 * Every file descriptor DankC cares about (Wayland, D-Bus, PipeWire, the niri
 * socket, timers, the dankctl control socket) is registered here and serviced
 * from one thread. See docs/01-ARCHITECTURE.md.
 */
#ifndef DC_CORE_LOOP_H
#define DC_CORE_LOOP_H

#include <stdbool.h>
#include <stdint.h>

typedef struct dc_loop dc_loop;

/* Called when a registered fd is ready. `revents` is the poll(2) revents mask. */
typedef void (*dc_fd_cb)(int fd, uint32_t revents, void *user_data);

/* Called once at the top of every loop iteration, before poll() blocks.
 * Used to flush buffered output (e.g. wl_display_flush). */
typedef void (*dc_prepare_cb)(void *user_data);

dc_loop *dc_loop_create(void);
void dc_loop_destroy(dc_loop *loop);

/* Register `fd` for the given poll events (POLLIN, ...). Returns 0 on success,
 * negative on failure. The loop does not take ownership of `fd`. */
int dc_loop_add_fd(dc_loop *loop, int fd, short events, dc_fd_cb cb, void *user_data);
void dc_loop_remove_fd(dc_loop *loop, int fd);

void dc_loop_set_prepare(dc_loop *loop, dc_prepare_cb cb, void *user_data);

/* Register an additional prepare hook (multiple subsystems can each add one). */
int dc_loop_add_prepare(dc_loop *loop, dc_prepare_cb cb, void *user_data);

/* Block servicing fds until dc_loop_stop() is called. */
void dc_loop_run(dc_loop *loop);
void dc_loop_stop(dc_loop *loop);

#endif /* DC_CORE_LOOP_H */
