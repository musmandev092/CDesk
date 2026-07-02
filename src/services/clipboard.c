#include "services/clipboard.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"
#include "wayland/wl.h"

#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "wlr-data-control-unstable-v1-client-protocol.h"

#define DC_CLIP_MAX 32
#define DC_CLIP_ENTRY_MAX 65536 /* cap a single entry's stored size */

#define DC_MIME_UTF8 "text/plain;charset=utf-8"
#define DC_MIME_TEXT "text/plain"

struct dc_clipboard {
    dc_wayland *wl;
    struct dc_loop *loop;
    struct zwlr_data_control_device_v1 *device;
    char *entries[DC_CLIP_MAX]; /* ring, oldest at head */
    int count;
    int head;
    dc_clip_changed_cb cb;
    void *cb_data;
};

/* Per-offer: which text mimes it advertises. */
struct offer_state {
    bool has_utf8;
    bool has_text;
};

/* An in-flight read of the selection into memory. */
struct transfer {
    dc_clipboard *c;
    int fd;
    struct zwlr_data_control_offer_v1 *offer;
    char *buf;
    size_t len;
    size_t cap;
};

/* Store `text` as the newest history entry (dedup vs the current newest). */
static void store_entry(dc_clipboard *c, const char *text)
{
    if (!text || !*text)
        return;
    if (c->count > 0) {
        int newest = (c->head + c->count - 1) % DC_CLIP_MAX;
        if (strcmp(c->entries[newest], text) == 0)
            return; /* same as last copy */
    }
    char *dup = strdup(text);
    if (!dup)
        return;
    int idx;
    if (c->count < DC_CLIP_MAX) {
        idx = (c->head + c->count) % DC_CLIP_MAX;
        c->count++;
    } else {
        idx = c->head;
        free(c->entries[idx]);
        c->head = (c->head + 1) % DC_CLIP_MAX;
    }
    c->entries[idx] = dup;
    dc_debug("clipboard: stored %zu bytes (%d entries)", strlen(dup), c->count);
    if (c->cb)
        c->cb(c->cb_data);
}

static void transfer_finish(struct transfer *t)
{
    dc_loop_remove_fd(t->c->loop, t->fd);
    close(t->fd);
    if (t->buf) {
        t->buf[t->len] = '\0';
        store_entry(t->c, t->buf);
        free(t->buf);
    }
    if (t->offer)
        zwlr_data_control_offer_v1_destroy(t->offer);
    free(t);
}

/* Pipe is readable: accumulate until EOF, then store. */
static void transfer_read(int fd, uint32_t revents, void *data)
{
    DC_UNUSED(revents);
    struct transfer *t = data;
    for (;;) {
        if (t->len + 4096 > t->cap) {
            size_t ncap = t->cap ? t->cap * 2 : 8192;
            if (ncap > DC_CLIP_ENTRY_MAX)
                ncap = DC_CLIP_ENTRY_MAX;
            if (ncap <= t->len) { /* entry too big — stop reading */
                transfer_finish(t);
                return;
            }
            char *nb = realloc(t->buf, ncap);
            if (!nb) {
                transfer_finish(t);
                return;
            }
            t->buf = nb;
            t->cap = ncap;
        }
        ssize_t n = read(fd, t->buf + t->len, t->cap - t->len - 1);
        if (n > 0) {
            t->len += (size_t)n;
            continue;
        }
        if (n == 0) { /* EOF */
            transfer_finish(t);
            return;
        }
        return; /* EAGAIN / would block: wait for the next POLLIN */
    }
}

static void offer_handle_offer(void *data, struct zwlr_data_control_offer_v1 *offer,
                               const char *mime)
{
    DC_UNUSED(offer);
    struct offer_state *st = data;
    if (strcmp(mime, DC_MIME_UTF8) == 0)
        st->has_utf8 = true;
    else if (strcmp(mime, DC_MIME_TEXT) == 0)
        st->has_text = true;
}

static const struct zwlr_data_control_offer_v1_listener offer_listener = {
    .offer = offer_handle_offer,
};

static void device_handle_data_offer(void *data, struct zwlr_data_control_device_v1 *device,
                                     struct zwlr_data_control_offer_v1 *offer)
{
    DC_UNUSED(data);
    DC_UNUSED(device);
    struct offer_state *st = calloc(1, sizeof(*st));
    zwlr_data_control_offer_v1_add_listener(offer, &offer_listener, st);
}

/* A new selection is available: read its text into history. */
static void device_handle_selection(void *data, struct zwlr_data_control_device_v1 *device,
                                    struct zwlr_data_control_offer_v1 *offer)
{
    DC_UNUSED(device);
    dc_clipboard *c = data;
    if (!offer)
        return;

    struct offer_state *st = zwlr_data_control_offer_v1_get_user_data(offer);
    const char *mime = NULL;
    if (st && st->has_utf8)
        mime = DC_MIME_UTF8;
    else if (st && st->has_text)
        mime = DC_MIME_TEXT;
    free(st);
    zwlr_data_control_offer_v1_set_user_data(offer, NULL);

    if (!mime) {
        zwlr_data_control_offer_v1_destroy(offer);
        return;
    }

    int fds[2];
    if (pipe(fds) < 0) {
        zwlr_data_control_offer_v1_destroy(offer);
        return;
    }
    zwlr_data_control_offer_v1_receive(offer, mime, fds[1]);
    close(fds[1]);
    wl_display_flush(c->wl->display); /* make sure the receive request is sent */

    struct transfer *t = calloc(1, sizeof(*t));
    t->c = c;
    t->fd = fds[0];
    t->offer = offer;
    dc_loop_add_fd(c->loop, fds[0], POLLIN, transfer_read, t);
}

static void device_handle_finished(void *data, struct zwlr_data_control_device_v1 *device)
{
    DC_UNUSED(data);
    zwlr_data_control_device_v1_destroy(device);
}

static void device_handle_primary_selection(void *data,
                                            struct zwlr_data_control_device_v1 *device,
                                            struct zwlr_data_control_offer_v1 *offer)
{
    DC_UNUSED(data);
    DC_UNUSED(device);
    if (offer) {
        struct offer_state *st = zwlr_data_control_offer_v1_get_user_data(offer);
        free(st);
        zwlr_data_control_offer_v1_destroy(offer); /* primary selection ignored */
    }
}

static const struct zwlr_data_control_device_v1_listener device_listener = {
    .data_offer = device_handle_data_offer,
    .selection = device_handle_selection,
    .finished = device_handle_finished,
    .primary_selection = device_handle_primary_selection,
};

dc_clipboard *dc_clipboard_create(dc_wayland *wl, struct dc_loop *loop)
{
    if (!wl->data_control_manager || !wl->seat) {
        dc_warn("no wlr-data-control; clipboard history disabled");
        return NULL;
    }
    dc_clipboard *c = calloc(1, sizeof(*c));
    c->wl = wl;
    c->loop = loop;
    c->device =
        zwlr_data_control_manager_v1_get_data_device(wl->data_control_manager, wl->seat);
    zwlr_data_control_device_v1_add_listener(c->device, &device_listener, c);
    dc_info("clipboard history watching selection");
    return c;
}

void dc_clipboard_destroy(dc_clipboard *c)
{
    if (!c)
        return;
    if (c->device)
        zwlr_data_control_device_v1_destroy(c->device);
    for (int i = 0; i < c->count; i++)
        free(c->entries[(c->head + i) % DC_CLIP_MAX]);
    free(c);
}

void dc_clipboard_set_changed_cb(dc_clipboard *c, dc_clip_changed_cb cb, void *user_data)
{
    if (!c)
        return;
    c->cb = cb;
    c->cb_data = user_data;
}

int dc_clipboard_history(dc_clipboard *c, const char **out, int max)
{
    if (!c)
        return 0;
    int n = 0;
    for (int i = c->count - 1; i >= 0 && n < max; i--)
        out[n++] = c->entries[(c->head + i) % DC_CLIP_MAX];
    return n;
}

int dc_clipboard_count(dc_clipboard *c)
{
    return c ? c->count : 0;
}

void dc_clipboard_copy(dc_clipboard *c, const char *text)
{
    DC_UNUSED(c);
    if (!text)
        return;
    /* Hand off to wl-copy, detached: robust and avoids owning a data source. */
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execlp("wl-copy", "wl-copy", "--", text, (char *)NULL);
        _exit(127);
    }
}
