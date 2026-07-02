#include "ui/notif_image.h"

#include "nanovg.h"

#include <stdio.h>
#include <string.h>

/* One entry per notification id that's ever had an image resolved. Sized
 * with headroom over DC_NOTIF_MAX since a notification can briefly exist in
 * both this cache and be mid-transition between Current/History during a
 * render pass. */
#define NOTIF_IMG_CACHE_MAX (DC_NOTIF_MAX + 8)

typedef struct {
    bool in_use;
    uint32_t id;
    int handle; /* nvg image handle, 0 = "resolved to nothing" (still cached to skip re-resolving) */
    int w, h;
    uint32_t version;      /* dc_notification.image_version when handle was built from pixel data */
    char source[DC_NOTIF_ICON]; /* file path used when handle was built from a file; "" for pixel data */
} notif_img_entry;

static notif_img_entry g_cache[NOTIF_IMG_CACHE_MAX];

static notif_img_entry *find_entry(uint32_t id)
{
    for (int i = 0; i < NOTIF_IMG_CACHE_MAX; i++)
        if (g_cache[i].in_use && g_cache[i].id == id)
            return &g_cache[i];
    return NULL;
}

/* Reuse `id`'s own slot if present (caller already handled eviction of its
 * old handle), else the first free slot, else the first slot (best-effort --
 * the cache is sized to comfortably exceed DC_NOTIF_MAX so this shouldn't
 * happen in practice). */
static notif_img_entry *acquire_entry(uint32_t id)
{
    notif_img_entry *e = find_entry(id);
    if (e)
        return e;
    for (int i = 0; i < NOTIF_IMG_CACHE_MAX; i++)
        if (!g_cache[i].in_use)
            return &g_cache[i];
    return &g_cache[0];
}

int dc_notif_image_get(dc_render *render, const dc_notification *n, int *out_w, int *out_h)
{
    *out_w = 0;
    *out_h = 0;
    if (!render || !render->vg || !n)
        return 0;

    bool use_pixels = n->image_pixels != NULL && n->image_w > 0 && n->image_h > 0;
    /* image-path outranks app_icon (notifications.h); app_icon only counts
     * here when it's an absolute file path -- a bare XDG icon name (e.g.
     * "dialog-information") isn't something dc_render_load_icon() can open
     * directly, and re-running the full icon-theme search from a render pass
     * would be needlessly expensive for what's just a fallback avatar. */
    const char *path = NULL;
    if (!use_pixels) {
        if (n->image_path[0])
            path = n->image_path;
        else if (n->app_icon[0] == '/')
            path = n->app_icon;
    }

    notif_img_entry *e = find_entry(n->id);
    if (e) {
        bool stale;
        if (use_pixels)
            stale = e->version != n->image_version || e->source[0] != '\0';
        else if (path)
            stale = e->source[0] == '\0' || strcmp(e->source, path) != 0 || e->version != 0;
        else
            stale = e->handle != 0 || e->source[0] != '\0';

        if (!stale) {
            *out_w = e->w;
            *out_h = e->h;
            return e->handle;
        }
        if (e->handle > 0)
            nvgDeleteImage(render->vg, e->handle);
        e->handle = 0;
        e->w = e->h = 0;
        e->version = 0;
        e->source[0] = '\0';
    }

    if (!use_pixels && !path) {
        e = acquire_entry(n->id);
        e->in_use = true;
        e->id = n->id;
        e->handle = 0;
        e->w = e->h = 0;
        e->version = 0;
        e->source[0] = '\0';
        return 0;
    }

    int handle = 0, w = 0, h = 0;
    if (use_pixels) {
        handle = nvgCreateImageRGBA(render->vg, n->image_w, n->image_h, 0, n->image_pixels);
        w = n->image_w;
        h = n->image_h;
    } else {
        /* 96px request: comfortably covers the ~40px HiDPI avatar circle
         * toasts.c/notifcenter.c draw this into (SVGs rasterize at exactly
         * this size; PNGs load at native resolution regardless -- see
         * dc_render_load_icon()). */
        handle = dc_render_load_icon(render, path, 96);
        if (handle > 0)
            nvgImageSize(render->vg, handle, &w, &h);
    }

    e = acquire_entry(n->id);
    if (e->in_use && e->id != n->id && e->handle > 0)
        nvgDeleteImage(render->vg, e->handle); /* stole another id's slot -- don't leak its texture */
    e->in_use = true;
    e->id = n->id;
    e->handle = handle;
    e->w = w;
    e->h = h;
    e->version = use_pixels ? n->image_version : 0;
    snprintf(e->source, sizeof(e->source), "%s", (!use_pixels && path) ? path : "");

    *out_w = w;
    *out_h = h;
    return handle;
}

void dc_notif_image_gc(dc_render *render, dc_notifications *notifications)
{
    if (!render || !render->vg || !notifications)
        return;

    const dc_notification *live[DC_NOTIF_MAX];
    int nlive = dc_notifications_current(notifications, live, DC_NOTIF_MAX);
    const dc_notification *hist[DC_NOTIF_MAX];
    int nhist = dc_notifications_history(notifications, hist, DC_NOTIF_MAX);

    for (int i = 0; i < NOTIF_IMG_CACHE_MAX; i++) {
        notif_img_entry *e = &g_cache[i];
        if (!e->in_use)
            continue;
        bool found = false;
        for (int j = 0; j < nlive && !found; j++)
            if (live[j]->id == e->id)
                found = true;
        for (int j = 0; j < nhist && !found; j++)
            if (hist[j]->id == e->id)
                found = true;
        if (!found) {
            if (e->handle > 0)
                nvgDeleteImage(render->vg, e->handle);
            memset(e, 0, sizeof(*e));
        }
    }
}
