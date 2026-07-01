#include "core/loop.h"
#include "core/log.h"

#include <errno.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>

#define DC_LOOP_MAX_FDS 64
#define DC_LOOP_MAX_PREPARE 8

struct dc_loop_source {
    int fd;
    dc_fd_cb cb;
    void *user_data;
};

struct dc_loop {
    struct dc_loop_source sources[DC_LOOP_MAX_FDS];
    struct pollfd fds[DC_LOOP_MAX_FDS];
    int count;
    bool running;
    dc_prepare_cb prepares[DC_LOOP_MAX_PREPARE];
    void *prepare_data[DC_LOOP_MAX_PREPARE];
    int prepare_count;
};

dc_loop *dc_loop_create(void)
{
    dc_loop *loop = calloc(1, sizeof(*loop));
    return loop;
}

void dc_loop_destroy(dc_loop *loop)
{
    free(loop);
}

int dc_loop_add_fd(dc_loop *loop, int fd, short events, dc_fd_cb cb, void *user_data)
{
    if (loop->count >= DC_LOOP_MAX_FDS) {
        dc_error("event loop is full (%d fds)", DC_LOOP_MAX_FDS);
        return -1;
    }

    int i = loop->count++;
    loop->sources[i].fd = fd;
    loop->sources[i].cb = cb;
    loop->sources[i].user_data = user_data;
    loop->fds[i].fd = fd;
    loop->fds[i].events = events;
    loop->fds[i].revents = 0;
    return 0;
}

void dc_loop_remove_fd(dc_loop *loop, int fd)
{
    for (int i = 0; i < loop->count; i++) {
        if (loop->sources[i].fd != fd)
            continue;
        int last = --loop->count;
        loop->sources[i] = loop->sources[last];
        loop->fds[i] = loop->fds[last];
        return;
    }
}

int dc_loop_add_prepare(dc_loop *loop, dc_prepare_cb cb, void *user_data)
{
    if (loop->prepare_count >= DC_LOOP_MAX_PREPARE) {
        dc_error("too many prepare hooks");
        return -1;
    }
    loop->prepares[loop->prepare_count] = cb;
    loop->prepare_data[loop->prepare_count] = user_data;
    loop->prepare_count++;
    return 0;
}

void dc_loop_set_prepare(dc_loop *loop, dc_prepare_cb cb, void *user_data)
{
    dc_loop_add_prepare(loop, cb, user_data);
}

void dc_loop_run(dc_loop *loop)
{
    loop->running = true;
    while (loop->running) {
        for (int i = 0; i < loop->prepare_count; i++)
            loop->prepares[i](loop->prepare_data[i]);

        int n = poll(loop->fds, loop->count, -1);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            dc_error("poll failed: %s", strerror(errno));
            break;
        }

        /* Snapshot revents: callbacks may add/remove sources mid-iteration. */
        for (int i = 0; i < loop->count && n > 0; i++) {
            uint32_t revents = loop->fds[i].revents;
            if (revents == 0)
                continue;
            n--;
            loop->fds[i].revents = 0;
            loop->sources[i].cb(loop->sources[i].fd, revents, loop->sources[i].user_data);
        }
    }
}

void dc_loop_stop(dc_loop *loop)
{
    loop->running = false;
}
