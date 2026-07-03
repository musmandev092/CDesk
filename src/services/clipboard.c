#include "services/clipboard.h"

#include "core/log.h"
#include "core/loop.h"
#include "dc.h"
#include "wayland/wl.h"

#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Declarations only (no STB_IMAGE_IMPLEMENTATION here) -- the implementation
 * is compiled once into third_party/nanovg/nanovg.c and linked in via
 * dankc-thirdparty, so this just needs stbi_info_from_memory()'s prototype
 * for a cheap PNG/JPEG header parse (width/height, no pixel decode). */
#include "stb_image.h"

#include "cJSON.h"

#include "wlr-data-control-unstable-v1-client-protocol.h"

#define DC_CLIP_MAX 32
#define DC_CLIP_TEXT_MAX (64 * 1024)          /* cap a single text entry */
#define DC_CLIP_IMAGE_MAX (8 * 1024 * 1024)   /* cap a single image entry (task: skip anything bigger) */

#define DC_MIME_UTF8 "text/plain;charset=utf-8"
#define DC_MIME_TEXT "text/plain"
#define DC_MIME_PNG "image/png"
#define DC_MIME_JPEG "image/jpeg"

/* Internal storage for one history entry -- superset of the public
 * dc_clip_entry view (adds ownership + a fixed ext buffer). */
struct clip_item {
    uint64_t id;
    dc_clip_kind kind;
    bool pinned;

    char *text;
    size_t text_len;

    unsigned char *image;
    size_t image_len;
    int width, height;
    char ext[8];
};

struct dc_clipboard {
    dc_wayland *wl;
    struct dc_loop *loop;
    struct zwlr_data_control_device_v1 *device;

    /* oldest at [0], newest at [count-1]; plain array (not a ring) so
     * id-based delete/pin can memmove-compact in place. */
    struct clip_item entries[DC_CLIP_MAX];
    int count;
    uint64_t next_id;

    dc_clip_changed_cb cb;
    void *cb_data;
};

/* Per-offer: which mimes it advertises. Text is preferred over image when an
 * offer somehow has both (shouldn't normally happen). */
struct offer_state {
    bool has_utf8;
    bool has_text;
    bool has_png;
    bool has_jpeg;
};

/* An in-flight read of the selection into memory. */
struct transfer {
    dc_clipboard *c;
    int fd;
    struct zwlr_data_control_offer_v1 *offer;
    char *buf;
    size_t len;
    size_t cap;
    size_t cap_limit;
    bool is_image;
    bool overflowed; /* image offer exceeded cap_limit -- skip, don't store truncated data */
    char ext[8];
};

static void free_item(struct clip_item *e)
{
    free(e->text);
    e->text = NULL;
    free(e->image);
    e->image = NULL;
}

static int find_by_id(dc_clipboard *c, uint64_t id)
{
    for (int i = 0; i < c->count; i++)
        if (c->entries[i].id == id)
            return i;
    return -1;
}

/* Evict the oldest *unpinned* entry to make room. Returns false if the whole
 * history is pinned (nothing to evict -- new entry is dropped). */
static bool evict_oldest_unpinned(dc_clipboard *c)
{
    int idx = -1;
    for (int i = 0; i < c->count; i++) {
        if (!c->entries[i].pinned) {
            idx = i;
            break;
        }
    }
    if (idx < 0)
        return false;
    free_item(&c->entries[idx]);
    memmove(&c->entries[idx], &c->entries[idx + 1],
           (size_t)(c->count - idx - 1) * sizeof(c->entries[0]));
    c->count--;
    return true;
}

/* Pinned-text persistence (~/.local/state/dankc/clipboard_pins.json, cJSON).
 * Only text entries are saved -- images aren't persisted across restarts
 * (task doc: "document that images don't persist"), so a pinned image
 * survives eviction for the running session only. */
static bool pins_path(char *out, size_t n)
{
    const char *xdg = getenv("XDG_STATE_HOME");
    const char *home = getenv("HOME");
    if (xdg && *xdg)
        snprintf(out, n, "%.400s/dankc/clipboard_pins.json", xdg);
    else if (home)
        snprintf(out, n, "%.400s/.local/state/dankc/clipboard_pins.json", home);
    else
        return false;
    return true;
}

static void pins_ensure_parent_dir(const char *path)
{
    char dir[512];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash)
        return;
    *slash = '\0';
    char *slash2 = strrchr(dir, '/');
    if (slash2) {
        *slash2 = '\0';
        mkdir(dir, 0755);
        *slash2 = '/';
    }
    mkdir(dir, 0755);
}

static char *pins_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n > 1 << 20) { /* sanity cap: 1 MiB of pinned text */
        fclose(f);
        return NULL;
    }
    char *buf = malloc((size_t)n + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = '\0';
    return buf;
}

/* Save every currently-pinned *text* entry, newest-pinned first (same order
 * dc_clipboard_list() already returns pinned entries in), so a restart can
 * reconstruct the exact same relative order. */
static void save_pins(dc_clipboard *c)
{
    char path[512];
    if (!pins_path(path, sizeof(path)))
        return;
    pins_ensure_parent_dir(path);

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "pins");
    for (int i = c->count - 1; i >= 0; i--) {
        struct clip_item *e = &c->entries[i];
        if (e->pinned && e->kind == DC_CLIP_TEXT && e->text)
            cJSON_AddItemToArray(arr, cJSON_CreateString(e->text));
    }

    char *text = cJSON_Print(root);
    cJSON_Delete(root);
    if (!text)
        return;
    FILE *f = fopen(path, "w");
    if (f) {
        fputs(text, f);
        fputc('\n', f);
        fclose(f);
    } else {
        dc_warn("clipboard: could not write %s", path);
    }
    free(text);
}

/* Load pinned text saved by a previous run and reinsert it as pinned
 * history entries so it survives the restart. Called once at startup,
 * before the first live selection arrives. Entries are appended oldest-pin
 * first so dc_clipboard_list()'s "newest pinned first" pass reproduces the
 * saved order. */
static void load_pins(dc_clipboard *c)
{
    char path[512];
    if (!pins_path(path, sizeof(path)))
        return;
    char *text = pins_read_file(path);
    if (!text)
        return;
    cJSON *root = cJSON_Parse(text);
    free(text);
    if (!root)
        return;
    cJSON *arr = cJSON_GetObjectItemCaseSensitive(root, "pins");
    if (cJSON_IsArray(arr)) {
        int n = cJSON_GetArraySize(arr);
        for (int i = n - 1; i >= 0 && c->count < DC_CLIP_MAX; i--) {
            cJSON *item = cJSON_GetArrayItem(arr, i);
            if (!cJSON_IsString(item) || !item->valuestring || !item->valuestring[0])
                continue;
            char *dup = strdup(item->valuestring);
            if (!dup)
                continue;
            struct clip_item *e = &c->entries[c->count++];
            memset(e, 0, sizeof(*e));
            e->id = ++c->next_id;
            e->kind = DC_CLIP_TEXT;
            e->pinned = true;
            e->text = dup;
            e->text_len = strlen(dup);
        }
    }
    cJSON_Delete(root);
    if (c->count > 0)
        dc_info("clipboard: restored %d pinned text entr%s from %s", c->count,
               c->count == 1 ? "y" : "ies", path);
}

/* Store `text` as the newest history entry (dedup vs the current newest). */
static void store_text(dc_clipboard *c, const char *text)
{
    if (!text || !*text)
        return;
    if (c->count > 0) {
        struct clip_item *newest = &c->entries[c->count - 1];
        if (newest->kind == DC_CLIP_TEXT && strcmp(newest->text, text) == 0)
            return; /* same as last copy */
    }
    char *dup = strdup(text);
    if (!dup)
        return;
    if (c->count >= DC_CLIP_MAX && !evict_oldest_unpinned(c)) {
        free(dup);
        return;
    }
    struct clip_item *e = &c->entries[c->count++];
    memset(e, 0, sizeof(*e));
    e->id = ++c->next_id;
    e->kind = DC_CLIP_TEXT;
    e->text = dup;
    e->text_len = strlen(dup);
    dc_debug("clipboard: stored %zu bytes of text (%d entries)", e->text_len, c->count);
    if (c->cb)
        c->cb(c->cb_data);
}

/* Store an already-decoded image buffer as the newest entry. Takes ownership
 * of `data` in every case (frees it if not stored). */
static void store_image(dc_clipboard *c, unsigned char *data, size_t len, const char *ext)
{
    if (!data || len == 0) {
        free(data);
        return;
    }
    if (c->count > 0) {
        struct clip_item *newest = &c->entries[c->count - 1];
        if (newest->kind == DC_CLIP_IMAGE && newest->image_len == len &&
            memcmp(newest->image, data, len) == 0) {
            free(data);
            return; /* same as last copy */
        }
    }
    if (c->count >= DC_CLIP_MAX && !evict_oldest_unpinned(c)) {
        free(data);
        return;
    }
    struct clip_item *e = &c->entries[c->count++];
    memset(e, 0, sizeof(*e));
    e->id = ++c->next_id;
    e->kind = DC_CLIP_IMAGE;
    e->image = data;
    e->image_len = len;
    snprintf(e->ext, sizeof(e->ext), "%s", (ext && ext[0]) ? ext : "png");

    int w = 0, h = 0, comp = 0;
    if (stbi_info_from_memory(data, (int)len, &w, &h, &comp)) {
        e->width = w;
        e->height = h;
    }
    dc_debug("clipboard: stored image %zu bytes (%dx%d %s, %d entries)", len, e->width, e->height,
            e->ext, c->count);
    if (c->cb)
        c->cb(c->cb_data);
}

static void transfer_finish(struct transfer *t)
{
    dc_loop_remove_fd(t->c->loop, t->fd);
    close(t->fd);
    if (t->overflowed) {
        dc_debug("clipboard: image offer exceeds %zu-byte cap, skipping (task: skip >8MB)",
                t->cap_limit);
        free(t->buf);
    } else if (t->buf) {
        if (t->is_image) {
            store_image(t->c, (unsigned char *)t->buf, t->len, t->ext);
            t->buf = NULL; /* ownership moved into store_image() */
        } else {
            t->buf[t->len] = '\0';
            store_text(t->c, t->buf);
            free(t->buf);
        }
    }
    if (t->offer)
        zwlr_data_control_offer_v1_destroy(t->offer);
    free(t);
}

/* Pipe is readable: accumulate until EOF, then store. Once the buffer has
 * grown to cap_limit and still can't fit more, a naive "read() returned 0 ==
 * EOF" check misfires: read() with a 0-byte request also returns 0, which
 * would otherwise be mistaken for real EOF and silently store a
 * truncated/corrupt blob for any offer bigger than the cap. Instead, probe
 * with a real (1-byte) read to tell "exactly cap_limit bytes, true EOF" apart
 * from "more data follows" -- for images the latter means the whole entry
 * must be skipped (task: "skip >8MB"), not truncated; text keeps the older
 * truncate-at-cap behavior since a partial paste is still usable. */
static void transfer_read(int fd, uint32_t revents, void *data)
{
    DC_UNUSED(revents);
    struct transfer *t = data;
    for (;;) {
        size_t avail = (t->cap > t->len + 1) ? (t->cap - t->len - 1) : 0;
        if (avail == 0) {
            if (t->cap >= t->cap_limit) {
                if (!t->is_image) { /* text: cap reached, truncate in place */
                    transfer_finish(t);
                    return;
                }
                char probe;
                ssize_t pn = read(fd, &probe, 1);
                if (pn > 0) {
                    t->overflowed = true;
                    transfer_finish(t);
                } else if (pn == 0) { /* true EOF right at the cap */
                    transfer_finish(t);
                } /* else EAGAIN: wait for the next POLLIN and probe again */
                return;
            }
            size_t ncap = t->cap ? t->cap * 2 : 8192;
            if (ncap > t->cap_limit)
                ncap = t->cap_limit;
            char *nb = realloc(t->buf, ncap);
            if (!nb) {
                transfer_finish(t);
                return;
            }
            t->buf = nb;
            t->cap = ncap;
            continue;
        }
        ssize_t n = read(fd, t->buf + t->len, avail);
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
    else if (strcmp(mime, DC_MIME_PNG) == 0)
        st->has_png = true;
    else if (strcmp(mime, DC_MIME_JPEG) == 0)
        st->has_jpeg = true;
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

/* A new selection is available: read its text or image into history. Text
 * mimes are preferred over image mimes when both are offered. */
static void device_handle_selection(void *data, struct zwlr_data_control_device_v1 *device,
                                    struct zwlr_data_control_offer_v1 *offer)
{
    DC_UNUSED(device);
    dc_clipboard *c = data;
    if (!offer)
        return;

    struct offer_state *st = zwlr_data_control_offer_v1_get_user_data(offer);
    const char *mime = NULL;
    bool is_image = false;
    const char *ext = NULL;
    if (st && st->has_utf8) {
        mime = DC_MIME_UTF8;
    } else if (st && st->has_text) {
        mime = DC_MIME_TEXT;
    } else if (st && st->has_png) {
        mime = DC_MIME_PNG;
        is_image = true;
        ext = "png";
    } else if (st && st->has_jpeg) {
        mime = DC_MIME_JPEG;
        is_image = true;
        ext = "jpg";
    }
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
    t->is_image = is_image;
    t->cap_limit = is_image ? DC_CLIP_IMAGE_MAX : DC_CLIP_TEXT_MAX;
    if (is_image)
        snprintf(t->ext, sizeof(t->ext), "%s", ext);
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
    load_pins(c);
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
        free_item(&c->entries[i]);
    free(c);
}

void dc_clipboard_set_changed_cb(dc_clipboard *c, dc_clip_changed_cb cb, void *user_data)
{
    if (!c)
        return;
    c->cb = cb;
    c->cb_data = user_data;
}

static void fill_public(dc_clip_entry *out, const struct clip_item *e)
{
    out->id = e->id;
    out->kind = e->kind;
    out->pinned = e->pinned;
    out->text = e->text;
    out->text_len = e->text_len;
    out->image_data = e->image;
    out->image_len = e->image_len;
    out->width = e->width;
    out->height = e->height;
    out->image_ext = (e->kind == DC_CLIP_IMAGE) ? e->ext : NULL;
}

int dc_clipboard_list(dc_clipboard *c, dc_clip_entry *out, int max)
{
    if (!c)
        return 0;
    int n = 0;
    for (int i = c->count - 1; i >= 0 && n < max; i--)
        if (c->entries[i].pinned)
            fill_public(&out[n++], &c->entries[i]);
    for (int i = c->count - 1; i >= 0 && n < max; i--)
        if (!c->entries[i].pinned)
            fill_public(&out[n++], &c->entries[i]);
    return n;
}

int dc_clipboard_count(dc_clipboard *c)
{
    return c ? c->count : 0;
}

void dc_clipboard_copy(dc_clipboard *c, uint64_t id)
{
    if (!c)
        return;
    int idx = find_by_id(c, id);
    if (idx < 0)
        return;
    struct clip_item *e = &c->entries[idx];

    if (e->kind == DC_CLIP_TEXT) {
        /* Hand off to wl-copy, detached: robust and avoids owning a data
         * source. Text fits comfortably in argv. */
        pid_t pid = fork();
        if (pid == 0) {
            setsid();
            execlp("wl-copy", "wl-copy", "--", e->text, (char *)NULL);
            _exit(127);
        }
        return;
    }

    /* Image: binary data can't go through argv, so stage it in a temp file
     * and let a detached shell pipe it into wl-copy (then clean up). */
    char path[] = "/tmp/dankc-clip-XXXXXX";
    int fd = mkstemp(path);
    if (fd < 0)
        return;
    size_t off = 0;
    while (off < e->image_len) {
        ssize_t n = write(fd, e->image + off, e->image_len - off);
        if (n <= 0)
            break;
        off += (size_t)n;
    }
    close(fd);

    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        char mime[32];
        snprintf(mime, sizeof(mime), "image/%s", strcmp(e->ext, "jpg") == 0 ? "jpeg" : e->ext);
        char cmd[512];
        snprintf(cmd, sizeof(cmd), "wl-copy --type '%s' < '%s'; rm -f '%s'", mime, path, path);
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

void dc_clipboard_delete(dc_clipboard *c, uint64_t id)
{
    if (!c)
        return;
    int idx = find_by_id(c, id);
    if (idx < 0)
        return;
    bool was_pinned_text = c->entries[idx].pinned && c->entries[idx].kind == DC_CLIP_TEXT;
    free_item(&c->entries[idx]);
    memmove(&c->entries[idx], &c->entries[idx + 1],
           (size_t)(c->count - idx - 1) * sizeof(c->entries[0]));
    c->count--;
    if (was_pinned_text)
        save_pins(c);
    if (c->cb)
        c->cb(c->cb_data);
}

void dc_clipboard_toggle_pin(dc_clipboard *c, uint64_t id)
{
    if (!c)
        return;
    int idx = find_by_id(c, id);
    if (idx < 0)
        return;
    c->entries[idx].pinned = !c->entries[idx].pinned;
    dc_debug("clipboard: entry id=%llu %s", (unsigned long long)id,
            c->entries[idx].pinned ? "pinned" : "unpinned");
    if (c->entries[idx].kind == DC_CLIP_TEXT)
        save_pins(c);
    if (c->cb)
        c->cb(c->cb_data);
}

void dc_clipboard_clear_all(dc_clipboard *c)
{
    if (!c)
        return;
    int w = 0;
    for (int i = 0; i < c->count; i++) {
        if (c->entries[i].pinned) {
            if (w != i)
                c->entries[w] = c->entries[i];
            w++;
        } else {
            free_item(&c->entries[i]);
        }
    }
    c->count = w;
    if (c->cb)
        c->cb(c->cb_data);
}
