#include "services/greetd.h"

#include "cJSON.h"
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

/* greetd frames a length-prefixed native-endian uint32 in front of every
 * JSON payload (docs/28-GREETER-PLAN.md §greetd-wire-proto). A greeting the
 * daemon sends (auth_message text, error description) is bounded in
 * practice, but nothing in the protocol caps it — guard against a hostile or
 * buggy peer forcing an unbounded allocation. */
#define DC_GREETD_MAX_FRAME (1u << 20) /* 1 MiB */

/* Growable byte buffer used for both the outgoing write queue and the
 * incoming read accumulator. */
struct dc_greetd_buf {
    unsigned char *data;
    size_t len;
    size_t cap;
};

struct dc_greetd {
    struct dc_loop *loop;
    int fd;
    dc_greetd_event_cb cb;
    void *user_data;
    struct dc_greetd_buf rx;
    struct dc_greetd_buf tx; /* pending write bytes when the socket applies backpressure */
    bool dead;               /* fd hit EOF/error; further calls are no-ops */
};

static bool buf_reserve(struct dc_greetd_buf *b, size_t extra)
{
    if (b->len + extra <= b->cap)
        return true;
    size_t want = b->cap ? b->cap * 2 : 256;
    while (want < b->len + extra)
        want *= 2;
    unsigned char *n = realloc(b->data, want);
    if (!n)
        return false;
    b->data = n;
    b->cap = want;
    return true;
}

static bool buf_append(struct dc_greetd_buf *b, const void *p, size_t n)
{
    if (!buf_reserve(b, n))
        return false;
    memcpy(b->data + b->len, p, n);
    b->len += n;
    return true;
}

/* Drop the first `n` consumed bytes, sliding the remainder down. */
static void buf_consume(struct dc_greetd_buf *b, size_t n)
{
    if (n >= b->len) {
        b->len = 0;
        return;
    }
    memmove(b->data, b->data + n, b->len - n);
    b->len -= n;
}

static void greetd_fd_ready(int fd, uint32_t revents, void *user_data);

/* Try to flush as much of `tx` as the socket accepts right now. Called after
 * queuing a request and again once the fd reports POLLOUT. Non-blocking:
 * EAGAIN just leaves the remainder queued for next time. */
static void greetd_flush_tx(dc_greetd *g)
{
    while (g->tx.len > 0) {
        ssize_t n = write(g->fd, g->tx.data, g->tx.len);
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
                break;
            dc_warn("greetd: write: %s", strerror(errno));
            g->dead = true;
            break;
        }
        if (n == 0)
            break;
        buf_consume(&g->tx, (size_t)n);
    }

    if (g->dead)
        return; /* caller (greetd_fd_ready) removes the fd once it notices */

    /* Toggle POLLOUT registration based on whether bytes are still queued —
     * a socket is almost always write-ready, so leaving POLLOUT registered
     * unconditionally would spin the event loop's poll() with no blocking. */
    short events = POLLIN | (g->tx.len > 0 ? POLLOUT : 0);
    dc_loop_remove_fd(g->loop, g->fd);
    dc_loop_add_fd(g->loop, g->fd, events, greetd_fd_ready, g);
}

/* Frame `json` (native-u32 length prefix, no NUL) and queue/send it. */
static bool greetd_send_json(dc_greetd *g, cJSON *json)
{
    if (g->dead)
        return false;

    char *text = cJSON_PrintUnformatted(json);
    if (!text)
        return false;

    uint32_t len = (uint32_t)strlen(text);
    bool ok = buf_append(&g->tx, &len, sizeof(len)) && buf_append(&g->tx, text, len);
    cJSON_free(text);
    if (!ok)
        return false;

    greetd_flush_tx(g);
    return !g->dead;
}

/* Map one decoded response object to a dc_greetd_event and hand it to the
 * caller's callback. */
static void greetd_dispatch(dc_greetd *g, cJSON *root)
{
    const cJSON *type = cJSON_GetObjectItemCaseSensitive(root, "type");
    if (!cJSON_IsString(type) || !type->valuestring) {
        dc_warn("greetd: response with no type");
        return;
    }

    struct dc_greetd_event ev = {0};

    if (strcmp(type->valuestring, "success") == 0) {
        ev.kind = DC_GREETD_SUCCESS;
    } else if (strcmp(type->valuestring, "error") == 0) {
        const cJSON *err_type = cJSON_GetObjectItemCaseSensitive(root, "error_type");
        const cJSON *desc = cJSON_GetObjectItemCaseSensitive(root, "description");
        ev.kind = DC_GREETD_ERROR;
        if (cJSON_IsString(err_type) && err_type->valuestring)
            snprintf(ev.error_type, sizeof(ev.error_type), "%s", err_type->valuestring);
        if (cJSON_IsString(desc) && desc->valuestring)
            snprintf(ev.text, sizeof(ev.text), "%s", desc->valuestring);
    } else if (strcmp(type->valuestring, "auth_message") == 0) {
        const cJSON *amt = cJSON_GetObjectItemCaseSensitive(root, "auth_message_type");
        const cJSON *msg = cJSON_GetObjectItemCaseSensitive(root, "auth_message");
        ev.kind = DC_GREETD_AUTH_MESSAGE;
        ev.auth_type = DC_GREETD_AUTH_VISIBLE;
        if (cJSON_IsString(amt) && amt->valuestring) {
            if (strcmp(amt->valuestring, "secret") == 0)
                ev.auth_type = DC_GREETD_AUTH_SECRET;
            else if (strcmp(amt->valuestring, "info") == 0)
                ev.auth_type = DC_GREETD_AUTH_INFO;
            else if (strcmp(amt->valuestring, "error") == 0)
                ev.auth_type = DC_GREETD_AUTH_ERROR;
        }
        if (cJSON_IsString(msg) && msg->valuestring)
            snprintf(ev.text, sizeof(ev.text), "%s", msg->valuestring);
    } else {
        dc_warn("greetd: unknown response type '%s'", type->valuestring);
        return;
    }

    g->cb(&ev, g->user_data);
}

/* Pull as many complete (u32 len + payload) frames out of `rx` as are
 * present, parsing and dispatching each. Handles partial frames (wait for
 * more data) and multiple queued frames in one read (loop). */
static void greetd_process_rx(dc_greetd *g)
{
    for (;;) {
        if (g->rx.len < sizeof(uint32_t))
            return;

        uint32_t frame_len;
        memcpy(&frame_len, g->rx.data, sizeof(frame_len));

        if (frame_len > DC_GREETD_MAX_FRAME) {
            dc_warn("greetd: response frame too large (%u bytes)", frame_len);
            g->dead = true;
            return;
        }

        size_t total = sizeof(uint32_t) + frame_len;
        if (g->rx.len < total)
            return; /* wait for the rest of this frame */

        cJSON *root = cJSON_ParseWithLength((const char *)g->rx.data + sizeof(uint32_t), frame_len);
        if (root) {
            greetd_dispatch(g, root);
            cJSON_Delete(root);
        } else {
            dc_warn("greetd: malformed JSON response");
        }

        buf_consume(&g->rx, total);
    }
}

static void greetd_fd_ready(int fd, uint32_t revents, void *user_data)
{
    dc_greetd *g = user_data;

    if (revents & POLLOUT)
        greetd_flush_tx(g);

    if (revents & POLLIN) {
        unsigned char chunk[4096];
        for (;;) {
            ssize_t n = read(fd, chunk, sizeof(chunk));
            if (n > 0) {
                if (!buf_append(&g->rx, chunk, (size_t)n)) {
                    dc_warn("greetd: out of memory buffering response");
                    g->dead = true;
                    break;
                }
                if ((size_t)n < sizeof(chunk))
                    break; /* drained the socket for now */
                continue;
            }
            if (n == 0) {
                dc_debug("greetd: connection closed by peer");
                g->dead = true;
                break;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                break;
            if (errno == EINTR)
                continue;
            dc_warn("greetd: read: %s", strerror(errno));
            g->dead = true;
            break;
        }
        greetd_process_rx(g);
    }

    if ((revents & (POLLHUP | POLLERR)) && !(revents & POLLIN))
        g->dead = true;

    if (g->dead) {
        dc_loop_remove_fd(g->loop, g->fd);
        return;
    }
}

dc_greetd *dc_greetd_create(struct dc_loop *loop, dc_greetd_event_cb cb, void *user_data)
{
    if (!cb)
        return NULL;

    const char *path = getenv("DANKC_GREETD_SOCK_PATH");
    if (!path || !*path)
        path = getenv("GREETD_SOCK");
    if (!path || !*path) {
        dc_warn("greetd: neither DANKC_GREETD_SOCK_PATH nor GREETD_SOCK is set");
        return NULL;
    }

    struct sockaddr_un addr = {.sun_family = AF_UNIX};
    if (strlen(path) >= sizeof(addr.sun_path)) {
        dc_warn("greetd: socket path too long: %s", path);
        return NULL;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);

    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) {
        dc_warn("greetd: socket: %s", strerror(errno));
        return NULL;
    }

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0 && errno != EINPROGRESS) {
        dc_warn("greetd: connect %s: %s", path, strerror(errno));
        close(fd);
        return NULL;
    }

    dc_greetd *g = calloc(1, sizeof(*g));
    if (!g) {
        close(fd);
        return NULL;
    }
    g->loop = loop;
    g->fd = fd;
    g->cb = cb;
    g->user_data = user_data;

    if (dc_loop_add_fd(loop, fd, POLLIN, greetd_fd_ready, g) < 0) {
        dc_warn("greetd: failed to register socket on event loop");
        close(fd);
        free(g);
        return NULL;
    }

    dc_info("greetd: connected via %s", path);
    return g;
}

void dc_greetd_destroy(dc_greetd *g)
{
    if (!g)
        return;
    dc_loop_remove_fd(g->loop, g->fd);
    close(g->fd);
    free(g->rx.data);
    free(g->tx.data);
    free(g);
}

bool dc_greetd_create_session(dc_greetd *g, const char *username)
{
    if (!g || g->dead)
        return false;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "create_session");
    cJSON_AddStringToObject(root, "username", username ? username : "");
    bool ok = greetd_send_json(g, root);
    cJSON_Delete(root);
    return ok;
}

bool dc_greetd_respond(dc_greetd *g, const char *response)
{
    if (!g || g->dead)
        return false;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "post_auth_message_response");
    if (response)
        cJSON_AddStringToObject(root, "response", response);
    else
        cJSON_AddNullToObject(root, "response");
    bool ok = greetd_send_json(g, root);
    cJSON_Delete(root);
    return ok;
}

static int count_strv(char *const v[])
{
    int n = 0;
    if (v)
        while (v[n])
            n++;
    return n;
}

bool dc_greetd_start_session(dc_greetd *g, char *const cmd[], char *const env[])
{
    if (!g || g->dead || !cmd)
        return false;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "start_session");
    cJSON_AddItemToObject(root, "cmd", cJSON_CreateStringArray((const char *const *)cmd, count_strv(cmd)));
    cJSON_AddItemToObject(root, "env", cJSON_CreateStringArray((const char *const *)env, count_strv(env)));
    bool ok = greetd_send_json(g, root);
    cJSON_Delete(root);
    return ok;
}

bool dc_greetd_cancel(dc_greetd *g)
{
    if (!g || g->dead)
        return false;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "cancel_session");
    bool ok = greetd_send_json(g, root);
    cJSON_Delete(root);
    return ok;
}
