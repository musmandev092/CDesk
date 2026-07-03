#include "ui/clip_picker.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "render/shape.h"
#include "services/clipboard.h"
#include "theme/theme.h"
#include "ui/hover.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

/* Declarations only (implementation lives once in third_party/nanovg/nanovg.c,
 * same convention as services/clipboard.c) -- used to decode+downscale
 * thumbnails ourselves instead of nvgCreateImageMem's full-resolution
 * texture, so a multi-megapixel screenshot doesn't become a multi-megapixel
 * GL texture just to show a 96px row thumbnail. */
#include "stb_image.h"

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Sized to match docs/13-POPOUTS-SPEC.md sec.4/7 ("Panel ~420x560
 * bottom-right"), the same convention as notifcenter.c's 420x560. */
#define DC_CP_WIDTH 420
#define DC_CP_HEIGHT 560
#define DC_SCALE_BASE 120
/* Inset from the screen's right edge when bar-adjacent (docs/13-POPOUTS-SPEC.md
 * sec.0/4: opens near the clipboard icon, effectively the bar's right cluster). */
#define DC_CP_SIDE_MARGIN 12

#define DC_CP_PAD 6.0f     /* outer gutter for the drop shadow */
#define DC_CP_RADIUS 14.0f
#define DC_CP_INSET 16.0f  /* left/right content inset inside the card */

#define DC_CP_HEADER_TOP 14.0f
#define DC_CP_HEADER_H 28.0f
#define DC_CP_SEARCH_GAP 12.0f
#define DC_CP_SEARCH_H 40.0f
#define DC_CP_LIST_GAP 12.0f
#define DC_CP_BOTTOM_PAD 14.0f

/* Row height 72 matches DMS's ClipboardConstants.qml itemHeight exactly;
 * gap 8 approximates its DankListView spacing (Theme.spacingXS). */
#define DC_CP_ROW_H 72.0f
#define DC_CP_ROW_GAP 8.0f
#define DC_CP_THUMB_W 56.0f
#define DC_CP_THUMB_H 48.0f
/* Decoded-thumbnail texture cap (longest side, px) -- task: "downscale to
 * ~96px" so the GL texture stays small regardless of the source image's
 * resolution. */
#define DC_CP_THUMB_MAX_PX 96

#define DC_CP_MAX_ENTRIES 32 /* mirrors DC_CLIP_MAX in services/clipboard.c */
#define DC_CP_QUERY_MAX 128
#define DC_CP_SCROLL_STEP 56.0f
/* Text entries at or under this length show the "Text" tag; longer ones show
 * "Long Text" -- matches DMS's ClipboardService.qml longTextThreshold. */
#define DC_CP_LONG_TEXT_THRESHOLD 200

/* Shared layout so cp_render (draw), handle_click (hit-test), and the
 * keyboard scroll-into-view logic all agree -- same convention as
 * controlcenter.c's cc_layout. */
typedef struct {
    float pad, ix, iw;
    float header_y, header_h;
    float search_y, search_h;
    float list_y0, list_y1, list_h;
} cp_layout;

static cp_layout cp_get_layout(float w, float h)
{
    cp_layout l;
    l.pad = DC_CP_PAD;
    l.ix = l.pad + DC_CP_INSET;
    l.iw = w - 2.0f * l.ix;
    l.header_y = l.pad + DC_CP_HEADER_TOP;
    l.header_h = DC_CP_HEADER_H;
    l.search_y = l.header_y + l.header_h + DC_CP_SEARCH_GAP;
    l.search_h = DC_CP_SEARCH_H;
    l.list_y0 = l.search_y + l.search_h + DC_CP_LIST_GAP;
    l.list_y1 = h - l.pad - DC_CP_BOTTOM_PAD;
    l.list_h = l.list_y1 - l.list_y0;
    return l;
}

/* Per-row hit-test rects, recomputed every render (same "record while
 * drawing" convention as notifcenter.c's nc_card_hit). */
typedef struct {
    uint64_t id;
    int result_index; /* index into p->results as of this render */
    float row_y0, row_y1;
    float pin_x0, pin_y0, pin_x1, pin_y1;
    float del_x0, del_y0, del_x1, del_y1;
} cp_row_hit;

/* Hover ids (docs/13-POPOUTS-SPEC.md sec.4: hover bg on rows + pin/delete
 * buttons, selection follows hover like DMS). Each row's sub-regions are
 * packed as CP_HOVER_ROW_BASE + hit-index*3 + {0:body,1:pin,2:delete}, same
 * dynamic-list convention as notifcenter.c's NC_HOVER_CARD_BASE. */
#define CP_HOVER_NONE 0
#define CP_HOVER_CLOSE 1
#define CP_HOVER_CLEAR 2
#define CP_HOVER_ROW_BASE 10

/* Decoded-thumbnail GL texture cache, keyed by entry id -- avoids re-decoding
 * a PNG/JPEG from scratch on every single frame of the (60fps) entrance
 * animation. Bounded to DC_CP_MAX_ENTRIES since that's the whole history's
 * cap; entries no longer in history are purged in cp_purge_thumbnails(). */
typedef struct {
    uint64_t id;
    int handle;
} cp_thumb;

struct dc_clip_picker {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_clipboard *clipboard;
    dc_output *output;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width, logical_height, scale120, phys_width, phys_height;

    char query[DC_CP_QUERY_MAX];
    dc_clip_entry all[DC_CP_MAX_ENTRIES];     /* full history, pinned-first/newest-first */
    int all_count;
    dc_clip_entry results[DC_CP_MAX_ENTRIES]; /* query-filtered subset of `all` */
    int result_count;
    int selected;
    float scroll;
    float scroll_max;

    cp_thumb thumbs[DC_CP_MAX_ENTRIES];
    int thumb_count;

    /* Header hit-test rects. */
    float clear_x0, clear_y0, clear_x1, clear_y1;
    float close_x0, close_y0, close_x1, close_y1;

    cp_row_hit hits[DC_CP_MAX_ENTRIES];
    int hit_count;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;
    bool visible, configured, egl_ready;

    /* Entrance/exit scale-and-fade pivot, bar-position-aware — see
     * controlcenter.c's identical field for the full rationale. */
    float anim_ox, anim_oy;

    /* Hover tracking (docs/13-POPOUTS-SPEC.md sec.4), same guard pattern as
     * bar.c's dc_bar_pointer_motion(). */
    int hover_id;
};

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

static void cp_render(dc_clip_picker *p);
static void cp_teardown(dc_clip_picker *p);

static void cp_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_clip_picker *p = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    p->frame_cb = NULL;
    if (!p->visible)
        return;
    if (dc_anim_active(&p->anim))
        cp_render(p);
    else if (p->closing)
        cp_teardown(p);
}
static const struct wl_callback_listener cp_frame_listener = {.done = cp_frame_done};

static void recompute_physical(dc_clip_picker *p)
{
    p->phys_width = (p->logical_width * p->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    p->phys_height = (p->logical_height * p->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Case-insensitive substring test. */
static bool contains_ci(const char *hay, const char *needle)
{
    if (!needle[0])
        return true;
    size_t nl = strlen(needle);
    for (const char *h = hay; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl)
            return true;
    }
    return false;
}

/* Drop every cached thumbnail whose entry id is no longer present in `all`
 * (deleted, evicted, or cleared while the picker was open) so the GL texture
 * cache doesn't grow without bound across a long-lived session. */
static void cp_purge_thumbnails(dc_clip_picker *p)
{
    if (p->thumb_count == 0)
        return;
    /* Deleting a GL texture needs this surface's EGL context current; only
     * true once cp_show()'s first render has run (thumb_count is always 0
     * before that, so this is unreachable pre-render anyway) -- guarded
     * rather than assumed, matching cp_teardown()'s equivalent guard. */
    bool can_delete = p->egl_ready && dc_egl_make_current(p->egl, &p->egl_window);
    int w = 0;
    for (int i = 0; i < p->thumb_count; i++) {
        bool alive = false;
        for (int j = 0; j < p->all_count; j++)
            if (p->all[j].id == p->thumbs[i].id) {
                alive = true;
                break;
            }
        if (alive) {
            p->thumbs[w++] = p->thumbs[i];
        } else if (can_delete) {
            nvgDeleteImage(p->render->vg, p->thumbs[i].handle);
        }
    }
    p->thumb_count = w;
}

/* Build the query-visible haystack for one entry: the raw text for text
 * entries, or "image <ext> <W>x<H>" for images (so typing "png" or "image"
 * matches, mirroring DMS's ClipboardService.getLauncherEntries() haystack). */
static void cp_build_haystack(const dc_clip_entry *e, char *out, size_t cap)
{
    if (e->kind == DC_CLIP_TEXT) {
        snprintf(out, cap, "%s", e->text ? e->text : "");
    } else {
        snprintf(out, cap, "image %s %dx%d", e->image_ext ? e->image_ext : "", e->width,
                 e->height);
    }
}

static void run_filter(dc_clip_picker *p)
{
    char q[DC_CP_QUERY_MAX];
    size_t i = 0;
    for (; p->query[i] && i + 1 < sizeof(q); i++)
        q[i] = (char)tolower((unsigned char)p->query[i]);
    q[i] = '\0';

    p->result_count = 0;
    for (int j = 0; j < p->all_count && p->result_count < DC_CP_MAX_ENTRIES; j++) {
        char hay[DC_CP_QUERY_MAX + 256];
        cp_build_haystack(&p->all[j], hay, sizeof(hay));
        if (contains_ci(hay, q))
            p->results[p->result_count++] = p->all[j];
    }
    p->selected = 0;
    p->scroll = 0.0f;
}

/* Re-pull the full history from the service (pinned-first/newest-first,
 * already sorted by dc_clipboard_list()) and re-apply the current query. */
static void refresh_all(dc_clip_picker *p)
{
    p->all_count = dc_clipboard_list(p->clipboard, p->all, DC_CP_MAX_ENTRIES);
    cp_purge_thumbnails(p);
    run_filter(p);
}

static void clamp_selection_visible(dc_clip_picker *p)
{
    if (p->selected < 0)
        p->selected = 0;
    if (p->selected >= p->result_count)
        p->selected = p->result_count - 1;
    if (p->selected < 0) {
        p->selected = 0;
        return;
    }
    cp_layout l = cp_get_layout((float)p->logical_width, (float)p->logical_height);
    float content_h = p->result_count > 0
                          ? (float)p->result_count * (DC_CP_ROW_H + DC_CP_ROW_GAP) - DC_CP_ROW_GAP
                          : 0.0f;
    float scroll_max = content_h > l.list_h ? content_h - l.list_h : 0.0f;
    float row_top = (float)p->selected * (DC_CP_ROW_H + DC_CP_ROW_GAP);
    float row_bot = row_top + DC_CP_ROW_H;
    if (row_top < p->scroll)
        p->scroll = row_top;
    else if (row_bot > p->scroll + l.list_h)
        p->scroll = row_bot - l.list_h;
    if (p->scroll < 0.0f)
        p->scroll = 0.0f;
    if (p->scroll > scroll_max)
        p->scroll = scroll_max;
}

/* Human-readable size, matching the reference screenshot's "KiB"/"MiB"
 * units (docs/13-POPOUTS-SPEC.md sec.4: "[[ image 79 KiB png 485x608 ]]"). */
static void format_size(size_t bytes, char *out, size_t cap)
{
    if (bytes < 1024)
        snprintf(out, cap, "%zu B", bytes);
    else if (bytes < 1024 * 1024)
        snprintf(out, cap, "%.0f KiB", (double)bytes / 1024.0);
    else
        snprintf(out, cap, "%.1f MiB", (double)bytes / (1024.0 * 1024.0));
}

/* Box-filter downscale of an RGBA buffer from (sw,sh) to (dw,dh). Every
 * destination pixel is the average of its source footprint, which for a
 * large shrink (e.g. a 4000px screenshot down to 96px) looks far better than
 * nearest-neighbor / lets the GPU's own mip/linear filtering do less work.
 * Returns a malloc'd dw*dh*4 buffer, or NULL on allocation failure. */
static unsigned char *downscale_rgba(const unsigned char *src, int sw, int sh, int dw, int dh)
{
    unsigned char *dst = malloc((size_t)dw * (size_t)dh * 4);
    if (!dst)
        return NULL;
    for (int y = 0; y < dh; y++) {
        int sy0 = (int)((int64_t)y * sh / dh);
        int sy1 = (int)((int64_t)(y + 1) * sh / dh);
        if (sy1 <= sy0)
            sy1 = sy0 + 1;
        if (sy1 > sh)
            sy1 = sh;
        for (int x = 0; x < dw; x++) {
            int sx0 = (int)((int64_t)x * sw / dw);
            int sx1 = (int)((int64_t)(x + 1) * sw / dw);
            if (sx1 <= sx0)
                sx1 = sx0 + 1;
            if (sx1 > sw)
                sx1 = sw;
            long r = 0, g = 0, b = 0, a = 0, n = 0;
            for (int yy = sy0; yy < sy1; yy++) {
                const unsigned char *row = src + (size_t)yy * (size_t)sw * 4;
                for (int xx = sx0; xx < sx1; xx++) {
                    const unsigned char *px = row + (size_t)xx * 4;
                    r += px[0];
                    g += px[1];
                    b += px[2];
                    a += px[3];
                    n++;
                }
            }
            if (n == 0)
                n = 1;
            unsigned char *o = dst + ((size_t)y * (size_t)dw + (size_t)x) * 4;
            o[0] = (unsigned char)(r / n);
            o[1] = (unsigned char)(g / n);
            o[2] = (unsigned char)(b / n);
            o[3] = (unsigned char)(a / n);
        }
    }
    return dst;
}

/* Look up (or lazily decode + downscale + cache) the GL texture for an image
 * entry. Returns 0 if decoding failed or the entry isn't an image. Decodes
 * to full-res RGBA via stb_image, then downscales to DC_CP_THUMB_MAX_PX on
 * the longest side (task: "downscale to ~96px, nvgCreateImageRGBA lazily")
 * before uploading, so history entries with large source images (e.g. a
 * multi-megapixel screenshot) don't each cost a multi-megapixel GL texture
 * just to render a 56x48 row thumbnail. */
static int cp_get_thumbnail(dc_clip_picker *p, const dc_clip_entry *e)
{
    if (e->kind != DC_CLIP_IMAGE || !e->image_data || e->image_len == 0)
        return 0;
    for (int i = 0; i < p->thumb_count; i++)
        if (p->thumbs[i].id == e->id)
            return p->thumbs[i].handle;

    int sw = 0, sh = 0, n = 0;
    unsigned char *img =
        stbi_load_from_memory(e->image_data, (int)e->image_len, &sw, &sh, &n, 4);
    if (!img)
        return 0;

    int dw = sw, dh = sh;
    int longest = sw > sh ? sw : sh;
    if (longest > DC_CP_THUMB_MAX_PX) {
        float scale = (float)DC_CP_THUMB_MAX_PX / (float)longest;
        dw = (int)((float)sw * scale + 0.5f);
        dh = (int)((float)sh * scale + 0.5f);
        if (dw < 1)
            dw = 1;
        if (dh < 1)
            dh = 1;
    }

    int handle = 0;
    if (dw == sw && dh == sh) {
        handle = nvgCreateImageRGBA(p->render->vg, sw, sh, 0, img);
    } else {
        unsigned char *small = downscale_rgba(img, sw, sh, dw, dh);
        if (small) {
            handle = nvgCreateImageRGBA(p->render->vg, dw, dh, 0, small);
            free(small);
        }
    }
    stbi_image_free(img);
    if (handle <= 0)
        return 0;

    if (p->thumb_count < DC_CP_MAX_ENTRIES) {
        p->thumbs[p->thumb_count++] = (cp_thumb){.id = e->id, .handle = handle};
    } else {
        dc_warn("clip_picker: thumbnail cache full, not caching entry %llu",
               (unsigned long long)e->id);
        nvgDeleteImage(p->render->vg, handle);
        return 0;
    }
    return handle;
}

/* Draw an image thumbnail "cover"-fit (like QML's Image.PreserveAspectCrop)
 * into a rounded box -- the fill path itself is the rounded rect, so corners
 * come out rounded with no separate clip needed. */
static void draw_image_thumb(NVGcontext *vg, int handle, int img_w, int img_h, float x, float y,
                             float w, float h)
{
    float pw = w, ph = h;
    if (img_w > 0 && img_h > 0) {
        float sx = w / (float)img_w, sy = h / (float)img_h;
        float scale = sx > sy ? sx : sy;
        pw = (float)img_w * scale;
        ph = (float)img_h * scale;
    }
    float ox = x - (pw - w) * 0.5f;
    float oy = y - (ph - h) * 0.5f;
    NVGpaint paint = nvgImagePattern(vg, ox, oy, pw, ph, 0.0f, handle, 1.0f);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 8.0f);
    nvgFillPaint(vg, paint);
    nvgFill(vg);
}

/* One clipboard entry row (docs/13-POPOUTS-SPEC.md sec.4): index badge,
 * thumbnail (images only), type tag + preview, pin + delete actions. */
static void draw_row(dc_clip_picker *p, const dc_clip_entry *e, int display_index, float x,
                     float y, float w, bool selected)
{
    NVGcontext *vg = p->render->vg;
    const dc_theme *t = dc_theme_current;

    if (p->hit_count >= DC_CP_MAX_ENTRIES)
        return;
    cp_row_hit *hit = &p->hits[p->hit_count++];
    memset(hit, 0, sizeof(*hit));
    hit->id = e->id;
    hit->result_index = display_index - 1;
    hit->row_y0 = y;
    hit->row_y1 = y + DC_CP_ROW_H;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, DC_CP_ROW_H, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    if (selected) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, w, DC_CP_ROW_H, 12.0f);
        nvgFillColor(vg, tc_alpha(t->primary, 40));
        nvgFill(vg);
    }
    if (e->pinned) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x + 0.75f, y + 0.75f, w - 1.5f, DC_CP_ROW_H - 1.5f, 12.0f);
        nvgStrokeColor(vg, tc_alpha(t->primary, 90));
        nvgStrokeWidth(vg, 1.5f);
        nvgStroke(vg);
    }

    /* Index badge. */
    const float badge_r = 12.0f;
    const float badge_cx = x + 12.0f + badge_r;
    const float badge_cy = y + DC_CP_ROW_H / 2.0f;
    nvgBeginPath(vg);
    nvgCircle(vg, badge_cx, badge_cy, badge_r);
    nvgFillColor(vg, tc_alpha(t->primary, 40));
    nvgFill(vg);
    char num[16];
    snprintf(num, sizeof(num), "%d", display_index);
    nvgFontFaceId(vg, p->render->font_ui);
    nvgFontSize(vg, 12.0f);
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->primary));
    nvgText(vg, badge_cx, badge_cy + 1.0f, num, NULL);

    /* Right-aligned pin + delete actions. */
    const float act_r = 12.0f;
    float act_cx = x + w - 14.0f - act_r;
    const float act_cy = badge_cy;
    dc_render_icon(p->render, DC_ICON_CLOSE, act_cx, act_cy, 15.0f, t->surface_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    hit->del_x0 = act_cx - act_r - 4.0f;
    hit->del_y0 = act_cy - act_r - 4.0f;
    hit->del_x1 = act_cx + act_r + 4.0f;
    hit->del_y1 = act_cy + act_r + 4.0f;
    act_cx -= (act_r * 2.0f + 14.0f);
    dc_render_icon(p->render, DC_ICON_PUSH_PIN, act_cx, act_cy, 15.0f,
                  e->pinned ? t->primary : t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    hit->pin_x0 = act_cx - act_r - 4.0f;
    hit->pin_y0 = act_cy - act_r - 4.0f;
    hit->pin_x1 = act_cx + act_r + 4.0f;
    hit->pin_y1 = act_cy + act_r + 4.0f;

    /* Content column: thumbnail (images only) + type tag / preview text. */
    const float content_x0 = badge_cx + badge_r + 12.0f;
    const float content_x1 = hit->pin_x0 - 10.0f;
    float text_x = content_x0;

    if (e->kind == DC_CLIP_IMAGE) {
        float thumb_y = y + (DC_CP_ROW_H - DC_CP_THUMB_H) / 2.0f;
        int handle = cp_get_thumbnail(p, e);
        if (handle > 0) {
            draw_image_thumb(vg, handle, e->width, e->height, content_x0, thumb_y, DC_CP_THUMB_W,
                             DC_CP_THUMB_H);
        } else {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, content_x0, thumb_y, DC_CP_THUMB_W, DC_CP_THUMB_H, 8.0f);
            nvgFillColor(vg, tc(t->surface_container_highest));
            nvgFill(vg);
            dc_render_icon(p->render, DC_ICON_IMAGE, content_x0 + DC_CP_THUMB_W / 2.0f,
                          thumb_y + DC_CP_THUMB_H / 2.0f, 20.0f, t->surface_variant_text,
                          NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }
        text_x = content_x0 + DC_CP_THUMB_W + 12.0f;
    }

    const float text_w = content_x1 - text_x;
    if (text_w <= 8.0f)
        return;

    char tag[24];
    char body[256];
    if (e->kind == DC_CLIP_IMAGE) {
        char size_buf[24];
        format_size(e->image_len, size_buf, sizeof(size_buf));
        snprintf(body, sizeof(body), "[[ image %s %s %dx%d ]]", size_buf,
                 e->image_ext ? e->image_ext : "img", e->width, e->height);
        snprintf(tag, sizeof(tag), "Image");
    } else {
        bool is_long = e->text_len > DC_CP_LONG_TEXT_THRESHOLD;
        snprintf(tag, sizeof(tag), "%s", is_long ? "Long Text" : "Text");
        snprintf(body, sizeof(body), "%s", e->text ? e->text : "");
    }

    nvgSave(vg);
    nvgScissor(vg, text_x, y + 6.0f, text_w, DC_CP_ROW_H - 12.0f);

    nvgFontFaceId(vg, p->render->font_ui);
    nvgFontSize(vg, 11.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
    if (e->kind == DC_CLIP_IMAGE) {
        char tag_line[300];
        snprintf(tag_line, sizeof(tag_line), "%s \xe2\x80\xa2 %s", tag, body);
        nvgFillColor(vg, tc(t->primary));
        nvgText(vg, text_x, y + 10.0f, tag_line, NULL);
    } else {
        nvgFillColor(vg, tc(t->primary));
        nvgText(vg, text_x, y + 10.0f, tag, NULL);
    }

    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextLineHeight(vg, 1.2f);
    dc_shape_draw_textbox(p->render, text_x, y + 27.0f, text_w, body, NULL);

    nvgRestore(vg);
}

/* Hover bg (docs/13-POPOUTS-SPEC.md sec.4; formula from bar.c's
 * draw_hover_overlay(), shared via hover.h): painted last, on top of
 * whatever's already drawn at that hit rect. A row body spans the full list
 * width at its own 12px corner radius; pin/delete/close/clear are all small
 * square hit rects, drawn as circles. `l` is the layout already computed by
 * the caller (cp_render()), so the row body's x-span doesn't need its own
 * per-row storage. */
static void draw_cp_hover(dc_clip_picker *p, const cp_layout *l)
{
    if (p->hover_id == CP_HOVER_NONE)
        return;

    float x0 = 0, y0 = 0, x1 = 0, y1 = 0, radius = 6.0f;

    if (p->hover_id == CP_HOVER_CLOSE) {
        x0 = p->close_x0;
        y0 = p->close_y0;
        x1 = p->close_x1;
        y1 = p->close_y1;
        radius = (x1 - x0) / 2.0f;
    } else if (p->hover_id == CP_HOVER_CLEAR) {
        x0 = p->clear_x0;
        y0 = p->clear_y0;
        x1 = p->clear_x1;
        y1 = p->clear_y1;
        radius = (x1 - x0) / 2.0f;
    } else if (p->hover_id >= CP_HOVER_ROW_BASE) {
        int rel = p->hover_id - CP_HOVER_ROW_BASE;
        int i = rel / 3, kind = rel % 3;
        if (i < 0 || i >= p->hit_count)
            return;
        const cp_row_hit *hit = &p->hits[i];
        switch (kind) {
        case 0:
            x0 = l->ix;
            y0 = hit->row_y0;
            x1 = l->ix + l->iw;
            y1 = hit->row_y1;
            radius = 12.0f;
            break;
        case 1:
            x0 = hit->pin_x0;
            y0 = hit->pin_y0;
            x1 = hit->pin_x1;
            y1 = hit->pin_y1;
            radius = (x1 - x0) / 2.0f;
            break;
        case 2:
            x0 = hit->del_x0;
            y0 = hit->del_y0;
            x1 = hit->del_x1;
            y1 = hit->del_y1;
            radius = (x1 - x0) / 2.0f;
            break;
        default:
            return;
        }
    } else {
        return;
    }
    if (x1 <= x0 || y1 <= y0)
        return;

    NVGcontext *vg = p->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    dc_color hc =
        dc_hover_bg_color(t->surface_container_high, t->primary, cfg->bar_widget_transparency);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x0, y0, x1 - x0, y1 - y0, radius);
    nvgFillColor(vg, nvgRGBA(hc.r, hc.g, hc.b, hc.a));
    nvgFill(vg);
}

static void cp_render(dc_clip_picker *p)
{
    if (!p->configured || p->phys_width <= 0)
        return;
    if (!p->egl_ready) {
        if (!dc_egl_window_init(&p->egl_window, p->egl, p->surface, p->phys_width, p->phys_height))
            return;
        p->egl_ready = true;
    } else {
        dc_egl_window_resize(&p->egl_window, p->phys_width, p->phys_height);
    }
    if (!dc_egl_make_current(p->egl, &p->egl_window))
        return;
    if (!dc_render_ensure(p->render))
        return;
    if (p->viewport)
        wp_viewport_set_destination(p->viewport, p->logical_width, p->logical_height);

    NVGcontext *vg = p->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = p->logical_width, h = p->logical_height;
    const float pad = DC_CP_PAD;

    glViewport(0, 0, p->phys_width, p->phys_height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)p->scale120 / DC_SCALE_BASE);

    float pr = dc_anim_progress(&p->anim);
    if (p->closing)
        pr = 1.0f - (pr > 1.0f ? 1.0f : pr);
    float alpha = pr > 1.0f ? 1.0f : pr;
    float scale = 0.94f + 0.06f * pr;
    float ox = pad + (w - 2.0f * pad) * p->anim_ox;
    float oy = pad + (h - 2.0f * pad) * p->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, DC_CP_RADIUS,
                                     18.0f, nvgRGBA(0, 0, 0, 100), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, DC_CP_RADIUS);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, DC_CP_RADIUS);
    nvgFillColor(vg, tc(t->surface_container));
    nvgFill(vg);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    cp_layout l = cp_get_layout(w, h);

    /* --- Header: clipboard icon + "Clipboard History (N)"; clear-all + X -- */
    const float header_cy = l.header_y + l.header_h / 2.0f;
    dc_render_icon(p->render, DC_ICON_CONTENT_PASTE, l.ix, header_cy, 19.0f, t->primary,
                  NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    char title[48];
    snprintf(title, sizeof(title), "Clipboard History (%d)", p->all_count);
    nvgFontFaceId(vg, p->render->font_ui);
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, l.ix + 28.0f, header_cy, title, NULL);

    const float hbtn_r = 12.0f;
    float hbtn_cx = l.ix + l.iw - hbtn_r;
    dc_render_icon(p->render, DC_ICON_CLOSE, hbtn_cx, header_cy, 16.0f, t->surface_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    p->close_x0 = hbtn_cx - hbtn_r - 6.0f;
    p->close_y0 = header_cy - hbtn_r - 6.0f;
    p->close_x1 = hbtn_cx + hbtn_r + 6.0f;
    p->close_y1 = header_cy + hbtn_r + 6.0f;

    hbtn_cx -= (hbtn_r * 2.0f + 16.0f);
    dc_render_icon(p->render, DC_ICON_DELETE_SWEEP, hbtn_cx, header_cy, 17.0f, t->surface_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    p->clear_x0 = hbtn_cx - hbtn_r - 6.0f;
    p->clear_y0 = header_cy - hbtn_r - 6.0f;
    p->clear_x1 = hbtn_cx + hbtn_r + 6.0f;
    p->clear_y1 = header_cy + hbtn_r + 6.0f;

    /* --- Search field: rounded, magnifier icon, green border (always
     * focused -- the layer surface grabs keyboard exclusively while open, so
     * there's no separate blurred state to represent). --------------------- */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l.ix, l.search_y, l.iw, l.search_h, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    nvgStrokeColor(vg, tc(t->primary));
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);
    const float scy = l.search_y + l.search_h / 2.0f;
    dc_render_icon(p->render, DC_ICON_SEARCH, l.ix + 16.0f, scy, 18.0f, t->surface_variant_text,
                  NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    const float sx = l.ix + 44.0f;
    nvgFontFaceId(vg, p->render->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    if (p->query[0]) {
        nvgFillColor(vg, tc(t->surface_text));
        dc_shape_draw_text(p->render, sx, scy, p->query, NULL);
    } else {
        nvgFillColor(vg, tc_alpha(t->surface_text, 110));
        nvgText(vg, sx, scy, "Search clipboard\xe2\x80\xa6", NULL);
    }

    /* --- Row list, scrollable ------------------------------------------- */
    float content_h = p->result_count > 0
                          ? (float)p->result_count * (DC_CP_ROW_H + DC_CP_ROW_GAP) - DC_CP_ROW_GAP
                          : 0.0f;
    p->scroll_max = content_h > l.list_h ? content_h - l.list_h : 0.0f;
    if (p->scroll < 0.0f)
        p->scroll = 0.0f;
    if (p->scroll > p->scroll_max)
        p->scroll = p->scroll_max;

    p->hit_count = 0;

    if (p->result_count == 0) {
        dc_color dim = t->surface_text;
        dim.a = 90;
        dc_render_icon(p->render, DC_ICON_CONTENT_PASTE, w / 2.0f,
                      l.list_y0 + l.list_h / 2.0f - 16.0f, 32.0f, dim,
                      NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, p->render->font_ui);
        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 120));
        nvgText(vg, w / 2.0f, l.list_y0 + l.list_h / 2.0f + 14.0f,
               p->query[0] ? "No matches" : "Clipboard history is empty", NULL);
    } else {
        nvgSave(vg);
        nvgScissor(vg, l.ix, l.list_y0, l.iw, l.list_h);
        for (int i = 0; i < p->result_count; i++) {
            float y = l.list_y0 + (float)i * (DC_CP_ROW_H + DC_CP_ROW_GAP) - p->scroll;
            if (y + DC_CP_ROW_H < l.list_y0 || y > l.list_y1)
                continue; /* fully outside the viewport -- skip drawing + hit-test */
            draw_row(p, &p->results[i], i + 1, l.ix, y, l.iw, i == p->selected);
        }
        nvgRestore(vg);

        if (p->scroll_max > 0.0f) {
            float track_x = l.ix + l.iw - 3.0f;
            float thumb_h = l.list_h * (l.list_h / content_h);
            if (thumb_h < 24.0f)
                thumb_h = 24.0f;
            float thumb_y = l.list_y0 + (l.list_h - thumb_h) * (p->scroll / p->scroll_max);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, track_x, thumb_y, 3.0f, thumb_h, 1.5f);
            nvgFillColor(vg, tc_alpha(t->outline, 140));
            nvgFill(vg);
        }
    }

    draw_cp_hover(p, &l);

    nvgEndFrame(vg);
    if ((dc_anim_active(&p->anim) || p->closing) && !p->frame_cb) {
        p->frame_cb = wl_surface_frame(p->surface);
        wl_callback_add_listener(p->frame_cb, &cp_frame_listener, p);
    }
    dc_egl_swap(p->egl, &p->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_clip_picker *p = data;
    DC_UNUSED(fs);
    p->scale120 = (int)scale;
    recompute_physical(p);
    cp_render(p);
}
static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_clip_picker *p = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    p->logical_width = width > 0 ? (int)width : DC_CP_WIDTH;
    p->logical_height = height > 0 ? (int)height : DC_CP_HEIGHT;
    p->configured = true;
    recompute_physical(p);
    cp_render(p);
}
static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_clip_picker *p = data;
    DC_UNUSED(surface);
    p->configured = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

/* History changed underneath us (e.g. a new copy landing while the picker is
 * open, or our own delete/pin/clear round-tripping through the service). */
static void cp_clipboard_changed(void *data)
{
    dc_clip_picker *p = data;
    if (!p->visible || p->closing)
        return;
    refresh_all(p);
    cp_render(p);
}

dc_clip_picker *dc_clip_picker_create(dc_wayland *wl, dc_egl *egl, dc_render *render,
                                      dc_clipboard *clipboard)
{
    dc_clip_picker *p = calloc(1, sizeof(*p));
    p->wl = wl;
    p->egl = egl;
    p->render = render;
    p->clipboard = clipboard;
    p->logical_width = DC_CP_WIDTH;
    p->logical_height = DC_CP_HEIGHT;
    p->scale120 = DC_SCALE_BASE;
    /* The picker is the history's only consumer, so it owns the service's
     * changed callback (nothing else registers one -- checked main.c). A
     * mutation while the picker is open invalidates every dc_clip_entry
     * pointer in p->all/p->results (services/clipboard.h), so re-pull +
     * re-render immediately rather than painting stale/freed pointers. */
    dc_clipboard_set_changed_cb(clipboard, cp_clipboard_changed, p);
    return p;
}

static void cp_show(dc_clip_picker *p, dc_output *output)
{
    p->output = output;
    p->configured = false;
    p->egl_ready = false;
    p->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    p->query[0] = '\0';
    refresh_all(p);
    dc_anim_start(&p->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    p->surface = wl_compositor_create_surface(p->wl->compositor);
    if (p->wl->fractional_scale_mgr) {
        p->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            p->wl->fractional_scale_mgr, p->surface);
        wp_fractional_scale_v1_add_listener(p->fractional_scale, &fractional_scale_listener, p);
    }
    if (p->wl->viewporter)
        p->viewport = wp_viewporter_get_viewport(p->wl->viewporter, p->surface);

    p->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        p->wl->layer_shell, p->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:clipboard");

    /* Bar-adjacent, right-aligned (docs/13-POPOUTS-SPEC.md sec.0/4). */
    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_END, DC_CP_SIDE_MARGIN);
    p->anim_ox = pa.origin_x;
    p->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(p->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(p->layer_surface, DC_CP_WIDTH, DC_CP_HEIGHT);
    zwlr_layer_surface_v1_set_margin(p->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        p->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(p->layer_surface, &layer_surface_listener, p);
    wl_surface_commit(p->surface);
    p->visible = true;
    p->closing = false;
    dc_debug("clipboard picker shown");
}

static void cp_teardown(dc_clip_picker *p)
{
    if (p->frame_cb) {
        wl_callback_destroy(p->frame_cb);
        p->frame_cb = NULL;
    }
    if (p->thumb_count > 0 && p->egl_ready && dc_egl_make_current(p->egl, &p->egl_window)) {
        for (int i = 0; i < p->thumb_count; i++)
            nvgDeleteImage(p->render->vg, p->thumbs[i].handle);
    }
    p->thumb_count = 0;
    if (p->egl_ready)
        dc_egl_window_finish(&p->egl_window, p->egl);
    if (p->viewport)
        wp_viewport_destroy(p->viewport);
    if (p->fractional_scale)
        wp_fractional_scale_v1_destroy(p->fractional_scale);
    if (p->layer_surface)
        zwlr_layer_surface_v1_destroy(p->layer_surface);
    if (p->surface)
        wl_surface_destroy(p->surface);
    p->egl_ready = false;
    p->configured = false;
    p->viewport = NULL;
    p->fractional_scale = NULL;
    p->layer_surface = NULL;
    p->surface = NULL;
    p->visible = false;
    p->closing = false;
    p->hover_id = CP_HOVER_NONE;
    dc_debug("clipboard picker hidden");
}

static void cp_begin_close(dc_clip_picker *p)
{
    if (!p->visible || p->closing)
        return;
    dc_anim_start(&p->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    p->closing = true;
    if (!dc_anim_active(&p->anim)) {
        cp_teardown(p);
        return;
    }
    cp_render(p);
}

void dc_clip_picker_toggle(dc_clip_picker *p, dc_output *output)
{
    if (p->visible)
        cp_begin_close(p);
    else
        cp_show(p, output);
}

void dc_clip_picker_hide(dc_clip_picker *p)
{
    cp_begin_close(p);
}

bool dc_clip_picker_visible(dc_clip_picker *p)
{
    return p->visible;
}

struct wl_surface *dc_clip_picker_surface(dc_clip_picker *p)
{
    return p->surface;
}

void dc_clip_picker_handle_key(dc_clip_picker *p, uint32_t keysym, const char *utf8)
{
    if (!p->visible || p->closing)
        return;
    switch (keysym) {
    case XKB_KEY_Escape:
        cp_begin_close(p);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (p->selected >= 0 && p->selected < p->result_count) {
            dc_clipboard_copy(p->clipboard, p->results[p->selected].id);
            cp_begin_close(p);
        }
        return;
    case XKB_KEY_BackSpace: {
        size_t n = strlen(p->query);
        if (n > 0) {
            p->query[n - 1] = '\0';
            run_filter(p);
        }
        break;
    }
    case XKB_KEY_Up:
        p->selected--;
        clamp_selection_visible(p);
        break;
    case XKB_KEY_Down:
        p->selected++;
        clamp_selection_visible(p);
        break;
    default:
        if (utf8 && utf8[0] && !((unsigned char)utf8[0] < 0x20) && (unsigned char)utf8[0] != 0x7f) {
            size_t n = strlen(p->query), add = strlen(utf8);
            if (n + add < sizeof(p->query)) {
                memcpy(p->query + n, utf8, add + 1);
                run_filter(p);
            }
        }
        break;
    }
    cp_render(p);
}

static inline bool in_rect(double x, double y, float x0, float y0, float x1, float y1)
{
    return x1 > x0 && x >= x0 && x <= x1 && y >= y0 && y <= y1;
}

/* Which interactive element (if any) sits under (x, y) -- shares the exact
 * hit boundaries dc_clip_picker_handle_click() dispatches against (same
 * discipline as controlcenter.c's cc_hittest() / notifcenter.c's
 * nc_hittest()): close/clear first, then each row's pin/delete before its
 * own body. */
static int cp_hittest(dc_clip_picker *p, double x, double y)
{
    if (in_rect(x, y, p->close_x0, p->close_y0, p->close_x1, p->close_y1))
        return CP_HOVER_CLOSE;
    if (in_rect(x, y, p->clear_x0, p->clear_y0, p->clear_x1, p->clear_y1))
        return CP_HOVER_CLEAR;

    for (int i = 0; i < p->hit_count; i++) {
        cp_row_hit *hit = &p->hits[i];
        if (in_rect(x, y, hit->pin_x0, hit->pin_y0, hit->pin_x1, hit->pin_y1))
            return CP_HOVER_ROW_BASE + i * 3 + 1;
        if (in_rect(x, y, hit->del_x0, hit->del_y0, hit->del_x1, hit->del_y1))
            return CP_HOVER_ROW_BASE + i * 3 + 2;
        if (y >= (double)hit->row_y0 && y <= (double)hit->row_y1)
            return CP_HOVER_ROW_BASE + i * 3 + 0;
    }
    return CP_HOVER_NONE;
}

void dc_clip_picker_handle_click(dc_clip_picker *p, double x, double y)
{
    if (!p->visible || p->closing)
        return;

    if (in_rect(x, y, p->close_x0, p->close_y0, p->close_x1, p->close_y1)) {
        cp_begin_close(p);
        return;
    }
    if (in_rect(x, y, p->clear_x0, p->clear_y0, p->clear_x1, p->clear_y1)) {
        dc_clipboard_clear_all(p->clipboard);
        refresh_all(p);
        cp_render(p);
        return;
    }

    for (int i = 0; i < p->hit_count; i++) {
        cp_row_hit *hit = &p->hits[i];
        if (in_rect(x, y, hit->pin_x0, hit->pin_y0, hit->pin_x1, hit->pin_y1)) {
            dc_clipboard_toggle_pin(p->clipboard, hit->id);
            refresh_all(p);
            cp_render(p);
            return;
        }
        if (in_rect(x, y, hit->del_x0, hit->del_y0, hit->del_x1, hit->del_y1)) {
            dc_clipboard_delete(p->clipboard, hit->id);
            refresh_all(p);
            cp_render(p);
            return;
        }
        if (y >= (double)hit->row_y0 && y <= (double)hit->row_y1) {
            dc_clipboard_copy(p->clipboard, hit->id);
            cp_begin_close(p);
            return;
        }
    }
}

/* Pointer motion over the panel (docs/13-POPOUTS-SPEC.md sec.4): hover
 * tracking, re-rendering only when the hovered id changes (same guard
 * pattern as bar.c/controlcenter.c/notifcenter.c). Hovering a row's body
 * also moves the keyboard selection onto it ("selection follows hover",
 * matching DMS's DankListView) -- pin/delete/close/clear hover don't touch
 * selection, only the row body itself does. */
void dc_clip_picker_handle_motion(dc_clip_picker *p, double x, double y)
{
    if (!p->visible || p->closing)
        return;

    int id = cp_hittest(p, x, y);
    if (id == p->hover_id)
        return;

    p->hover_id = id;
    if (id >= CP_HOVER_ROW_BASE) {
        int rel = id - CP_HOVER_ROW_BASE;
        int hit_i = rel / 3, kind = rel % 3;
        if (kind == 0 && hit_i >= 0 && hit_i < p->hit_count) {
            int ri = p->hits[hit_i].result_index;
            if (ri >= 0 && ri < p->result_count)
                p->selected = ri;
        }
    }
    dc_wayland_set_cursor(p->wl, id != CP_HOVER_NONE ? DC_CURSOR_POINTER : DC_CURSOR_DEFAULT);
    cp_render(p);
}

/* Pointer left the panel entirely: clear hover. */
void dc_clip_picker_handle_leave(dc_clip_picker *p)
{
    if (p->hover_id == CP_HOVER_NONE)
        return;
    p->hover_id = CP_HOVER_NONE;
    dc_wayland_set_cursor(p->wl, DC_CURSOR_DEFAULT);
    cp_render(p);
}

void dc_clip_picker_handle_scroll(dc_clip_picker *p, int steps_v)
{
    if (!p->visible || p->closing || steps_v == 0)
        return;
    float s = p->scroll + (float)steps_v * DC_CP_SCROLL_STEP;
    if (s < 0.0f)
        s = 0.0f;
    if (s > p->scroll_max)
        s = p->scroll_max;
    if (s == p->scroll)
        return;
    p->scroll = s;
    cp_render(p);
}

void dc_clip_picker_destroy(dc_clip_picker *p)
{
    if (!p)
        return;
    if (p->visible)
        cp_teardown(p);
    free(p);
}
