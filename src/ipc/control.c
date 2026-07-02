#include "ipc/control.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"

#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define DC_CONTROL_MAX_CLIENTS 8

struct dc_control {
    struct dc_loop *loop;
    int fd; /* listening socket */
    char path[108];
    dc_control_cmd_cb cb;
    void *user_data;
};

/* Build $XDG_RUNTIME_DIR/dankc.sock (falls back to /tmp). */
static void socket_path(char *out, size_t n)
{
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || !*dir)
        dir = "/tmp";
    snprintf(out, n, "%.90s/dankc.sock", dir);
}

/* Trim leading/trailing ASCII whitespace in place. */
static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n')
        s++;
    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

/* A connected client is readable: read one request, dispatch each line, close. */
static void client_ready(int fd, uint32_t revents, void *data)
{
    dc_control *c = data;
    DC_UNUSED(revents);

    char buf[1024];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) {
        buf[n] = '\0';
        for (char *line = strtok(buf, "\n"); line; line = strtok(NULL, "\n")) {
            char *cmd = trim(line);
            if (*cmd) {
                dc_debug("control: %s", cmd);
                c->cb(cmd, c->user_data);
            }
        }
    }
    dc_loop_remove_fd(c->loop, fd);
    close(fd);
}

/* The listening socket has a pending connection. */
static void accept_ready(int fd, uint32_t revents, void *data)
{
    dc_control *c = data;
    DC_UNUSED(revents);
    int client = accept(fd, NULL, NULL);
    if (client < 0)
        return;
    if (dc_loop_add_fd(c->loop, client, POLLIN, client_ready, c) < 0)
        close(client);
}

dc_control *dc_control_create(struct dc_loop *loop, dc_control_cmd_cb cb, void *user_data)
{
    dc_control *c = calloc(1, sizeof(*c));
    c->loop = loop;
    c->cb = cb;
    c->user_data = user_data;
    socket_path(c->path, sizeof(c->path));

    c->fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (c->fd < 0) {
        dc_warn("control socket: %s", strerror(errno));
        free(c);
        return NULL;
    }

    unlink(c->path); /* clear a stale socket from a previous run */
    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", c->path);
    if (bind(c->fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 || listen(c->fd, 4) < 0) {
        dc_warn("control bind/listen: %s", strerror(errno));
        close(c->fd);
        free(c);
        return NULL;
    }

    dc_loop_add_fd(loop, c->fd, POLLIN, accept_ready, c);
    dc_info("control socket at %s", c->path);
    return c;
}

void dc_control_destroy(dc_control *c)
{
    if (!c)
        return;
    dc_loop_remove_fd(c->loop, c->fd);
    close(c->fd);
    unlink(c->path);
    free(c);
}

int dc_control_send(const char *cmd)
{
    char path[108];
    socket_path(path, sizeof(path));

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return -1;

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "dankc: no running shell at %s\n", path);
        close(fd);
        return -1;
    }

    char line[512];
    int len = snprintf(line, sizeof(line), "%s\n", cmd);
    ssize_t wrote = write(fd, line, (size_t)len);
    close(fd);
    return wrote == len ? 0 : -1;
}
