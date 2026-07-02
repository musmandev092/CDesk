#include "ui/clip_picker.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/clipboard.h"
#include "theme/theme.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_CP_WIDTH 640
#define DC_CP_HEIGHT 520
#define DC_SCALE_BASE 120
#define DC_CP_PAD 6.0f
#define DC_CP_INSET 18.0f
#define DC_CP_SEARCH_H 46.0f
#define DC_CP_ROW_H 46.0f
#define DC_CP_LIST_Y 84.0f
#define DC_CP_MAX_ROWS 8
#define DC_CP_QUERY_MAX 128
#define DC_CP_MAX_ENTRIES 32
/* Inset from the screen's right edge when bar-adjacent (docs/13-POPOUTS-SPEC.md
 * sec.0/4: opens near the clipboard icon, effectively the bar's right cluster). */
#define DC_CP_SIDE_MARGIN 12

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
    const char *results[DC_CP_MAX_ENTRIES];
    int result_count;
    int selected;
    int scroll;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;
    bool visible, configured, egl_ready;

    /* Entrance/exit scale-and-fade pivot, bar-position-aware — see
     * controlcenter.c's identical field for the full rationale. */
    float anim_ox, anim_oy;
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

static void run_filter(dc_clip_picker *p)
{
    const char *all[DC_CP_MAX_ENTRIES];
    int n = dc_clipboard_history(p->clipboard, all, DC_CP_MAX_ENTRIES);
    char q[DC_CP_QUERY_MAX];
    size_t i = 0;
    for (; p->query[i] && i + 1 < sizeof(q); i++)
        q[i] = (char)tolower((unsigned char)p->query[i]);
    q[i] = '\0';

    p->result_count = 0;
    for (int j = 0; j < n && p->result_count < DC_CP_MAX_ENTRIES; j++)
        if (contains_ci(all[j], q))
            p->results[p->result_count++] = all[j];
    p->selected = 0;
    p->scroll = 0;
}

static void clamp_scroll(dc_clip_picker *p)
{
    if (p->selected < 0)
        p->selected = 0;
    if (p->selected >= p->result_count)
        p->selected = p->result_count - 1;
    if (p->selected < 0)
        p->selected = 0;
    if (p->selected < p->scroll)
        p->scroll = p->selected;
    else if (p->selected >= p->scroll + DC_CP_MAX_ROWS)
        p->scroll = p->selected - DC_CP_MAX_ROWS + 1;
}

/* One-line preview: collapse whitespace/newlines, cap length. */
static void preview(const char *src, char *out, size_t cap)
{
    size_t o = 0;
    bool space = false;
    for (const char *s = src; *s && o + 1 < cap; s++) {
        char ch = *s;
        if (ch == '\n' || ch == '\t' || ch == '\r' || ch == ' ') {
            if (!space && o > 0)
                out[o++] = ' ';
            space = true;
        } else {
            out[o++] = ch;
            space = false;
        }
    }
    out[o] = '\0';
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
    const float pad = DC_CP_PAD, ix = DC_CP_INSET, iw = w - 2.0f * ix;

    glViewport(0, 0, p->phys_width, p->phys_height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)p->scale120 / DC_SCALE_BASE);

    float pr = dc_anim_progress(&p->anim);
    if (p->closing)
        pr = 1.0f - (pr > 1.0f ? 1.0f : pr);
    float alpha = pr > 1.0f ? 1.0f : pr;
    float scale = 0.92f + 0.08f * pr;
    float ox = pad + (w - 2.0f * pad) * p->anim_ox;
    float oy = pad + (h - 2.0f * pad) * p->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 16.0f, 20.0f,
                                     nvgRGBA(0, 0, 0, 110), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 16.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 16.0f);
    nvgFillColor(vg, tc(t->surface_container));
    nvgFill(vg);

    /* Search field. */
    const float sy = 20.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, ix, sy, iw, DC_CP_SEARCH_H, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    const float scy = sy + DC_CP_SEARCH_H / 2.0f;
    dc_render_icon(p->render, DC_ICON_CONTENT_PASTE, ix + 16.0f, scy, 20.0f, t->surface_text,
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    const float tx = ix + 48.0f;
    nvgFontFaceId(vg, p->render->font_ui);
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    if (p->query[0]) {
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, tx, scy, p->query, NULL);
    } else {
        nvgFillColor(vg, tc_alpha(t->surface_text, 110));
        nvgText(vg, tx, scy, "Search clipboard…", NULL);
    }

    /* Rows. */
    int visible = p->result_count - p->scroll;
    if (visible > DC_CP_MAX_ROWS)
        visible = DC_CP_MAX_ROWS;
    for (int r = 0; r < visible; r++) {
        int idx = p->scroll + r;
        float ry = DC_CP_LIST_Y + r * DC_CP_ROW_H;
        if (idx == p->selected) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, ix, ry, iw, DC_CP_ROW_H - 6.0f, 10.0f);
            nvgFillColor(vg, tc_alpha(t->primary, 46));
            nvgFill(vg);
        }
        char line[160];
        preview(p->results[idx], line, sizeof(line));
        nvgSave(vg);
        nvgScissor(vg, ix + 14.0f, ry, iw - 28.0f, DC_CP_ROW_H - 6.0f);
        nvgFontSize(vg, 14.0f);
        nvgFillColor(vg, tc(t->surface_text));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, ix + 14.0f, ry + (DC_CP_ROW_H - 6.0f) / 2.0f, line, NULL);
        nvgRestore(vg);
    }
    if (p->result_count == 0) {
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 120));
        nvgText(vg, w / 2.0f, DC_CP_LIST_Y + 60.0f,
                p->query[0] ? "No matches" : "Clipboard history is empty", NULL);
    }

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
    return p;
}

static void cp_show(dc_clip_picker *p, dc_output *output)
{
    p->output = output;
    p->configured = false;
    p->egl_ready = false;
    p->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    p->query[0] = '\0';
    run_filter(p);
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
            dc_clipboard_copy(p->clipboard, p->results[p->selected]);
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
        clamp_scroll(p);
        break;
    case XKB_KEY_Down:
        p->selected++;
        clamp_scroll(p);
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

void dc_clip_picker_handle_click(dc_clip_picker *p, double x, double y)
{
    if (!p->visible || p->closing)
        return;
    DC_UNUSED(x);
    if (y < DC_CP_LIST_Y)
        return;
    int r = (int)((y - DC_CP_LIST_Y) / DC_CP_ROW_H);
    int idx = p->scroll + r;
    if (r >= 0 && r < DC_CP_MAX_ROWS && idx >= 0 && idx < p->result_count) {
        dc_clipboard_copy(p->clipboard, p->results[idx]);
        cp_begin_close(p);
    }
}

void dc_clip_picker_destroy(dc_clip_picker *p)
{
    if (!p)
        return;
    if (p->visible)
        cp_teardown(p);
    free(p);
}
