#include "ui/launcher.h"

#include "core/anim.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/apps.h"
#include "services/icons.h"
#include "theme/theme.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_LAUNCHER_WIDTH 600
#define DC_LAUNCHER_HEIGHT 520
#define DC_SCALE_BASE 120

#define DC_LAUNCHER_PAD 6.0f    /* shadow room */
#define DC_LAUNCHER_INSET 18.0f /* inner content margin */
#define DC_LAUNCHER_SEARCH_H 46.0f
#define DC_LAUNCHER_ROW_H 54.0f
#define DC_LAUNCHER_RESULTS_Y 84.0f
#define DC_LAUNCHER_MAX_ROWS 7
#define DC_LAUNCHER_QUERY_MAX 128

struct dc_launcher {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;
    dc_apps *apps;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width;
    int logical_height;
    int scale120;
    int phys_width;
    int phys_height;

    char query[DC_LAUNCHER_QUERY_MAX];
    const dc_app *results[64];
    int result_count;
    int selected;
    int scroll; /* first visible row */

    dc_anim anim;                 /* entrance (fade + scale) */
    struct wl_callback *frame_cb; /* pending frame callback while animating */

    bool visible;
    bool configured;
    bool egl_ready;
};

static void launcher_render(dc_launcher *l);

/* Frame callback: advance the entrance animation one frame. */
static void frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener frame_listener = {.done = frame_done};

static void frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_launcher *l = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    l->frame_cb = NULL;
    if (l->visible && dc_anim_active(&l->anim))
        launcher_render(l);
}

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

static void recompute_physical(dc_launcher *l)
{
    l->phys_width = (l->logical_width * l->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    l->phys_height = (l->logical_height * l->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void run_search(dc_launcher *l)
{
    l->result_count =
        dc_apps_search(l->apps, l->query, l->results,
                       (int)(sizeof(l->results) / sizeof(l->results[0])));
    l->selected = 0;
    l->scroll = 0;
}

/* Keep the selected row within the visible window. */
static void clamp_scroll(dc_launcher *l)
{
    if (l->selected < 0)
        l->selected = 0;
    if (l->selected >= l->result_count)
        l->selected = l->result_count - 1;
    if (l->selected < 0)
        l->selected = 0;

    if (l->selected < l->scroll)
        l->scroll = l->selected;
    else if (l->selected >= l->scroll + DC_LAUNCHER_MAX_ROWS)
        l->scroll = l->selected - DC_LAUNCHER_MAX_ROWS + 1;
}

static void launcher_render(dc_launcher *l)
{
    if (!l->configured || l->phys_width <= 0)
        return;
    if (!l->egl_ready) {
        if (!dc_egl_window_init(&l->egl_window, l->egl, l->surface, l->phys_width, l->phys_height))
            return;
        l->egl_ready = true;
    } else {
        dc_egl_window_resize(&l->egl_window, l->phys_width, l->phys_height);
    }
    if (!dc_egl_make_current(l->egl, &l->egl_window))
        return;
    if (!dc_render_ensure(l->render))
        return;
    if (l->viewport)
        wp_viewport_set_destination(l->viewport, l->logical_width, l->logical_height);

    NVGcontext *vg = l->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = l->logical_width;
    const float h = l->logical_height;
    const float pad = DC_LAUNCHER_PAD;
    const float ix = DC_LAUNCHER_INSET;
    const float iw = w - 2.0f * ix;

    glViewport(0, 0, l->phys_width, l->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)l->scale120 / DC_SCALE_BASE);

    /* Entrance animation: fade in + scale up from center (DMS spotlight). */
    float p = dc_anim_progress(&l->anim);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.92f + 0.08f * p;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, w / 2.0f, h / 2.0f);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -w / 2.0f, -h / 2.0f);

    /* Drop shadow + card. */
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
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* Search field. */
    const float sy = 20.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, ix, sy, iw, DC_LAUNCHER_SEARCH_H, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    const float scy = sy + DC_LAUNCHER_SEARCH_H / 2.0f;
    dc_render_icon(l->render, DC_ICON_SEARCH, ix + 16.0f, scy, 22.0f, t->surface_text,
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    const float tx = ix + 48.0f;
    nvgFontFaceId(vg, l->render->font_ui);
    nvgFontSize(vg, 16.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    if (l->query[0]) {
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, tx, scy, l->query, NULL);
        float bounds[4];
        nvgTextBounds(vg, tx, scy, l->query, NULL, bounds);
        nvgBeginPath(vg);
        nvgRect(vg, bounds[2] + 2.0f, scy - 10.0f, 2.0f, 20.0f);
        nvgFillColor(vg, tc(t->primary));
        nvgFill(vg);
    } else {
        nvgFillColor(vg, tc_alpha(t->surface_text, 110));
        nvgText(vg, tx, scy, "Search applications…", NULL);
    }

    /* Result rows. */
    int visible = l->result_count - l->scroll;
    if (visible > DC_LAUNCHER_MAX_ROWS)
        visible = DC_LAUNCHER_MAX_ROWS;

    for (int r = 0; r < visible; r++) {
        int idx = l->scroll + r;
        const dc_app *app = l->results[idx];
        float ry = DC_LAUNCHER_RESULTS_Y + r * DC_LAUNCHER_ROW_H;

        if (idx == l->selected) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, ix, ry, iw, DC_LAUNCHER_ROW_H - 6.0f, 10.0f);
            nvgFillColor(vg, tc_alpha(t->primary, 46));
            nvgFill(vg);
        }

        float row_cy = ry + (DC_LAUNCHER_ROW_H - 6.0f) / 2.0f;

        /* App icon (PNG/SVG), loaded per-render and freed after the frame. */
        int img = 0;
        char *icon_path = dc_icon_resolve(app->id, 36, 1);
        if (icon_path) {
            img = dc_render_load_icon(l->render, icon_path, 36);
            free(icon_path);
        }
        const float isz = 36.0f;
        const float icx = ix + 12.0f;
        if (img > 0) {
            NVGpaint pat =
                nvgImagePattern(vg, icx, row_cy - isz / 2.0f, isz, isz, 0.0f, img, 1.0f);
            nvgBeginPath(vg);
            nvgRoundedRect(vg, icx, row_cy - isz / 2.0f, isz, isz, 8.0f);
            nvgFillPaint(vg, pat);
            nvgFill(vg);
        } else {
            dc_render_icon(l->render, DC_ICON_APPS, icx + isz / 2.0f, row_cy, 26.0f,
                           t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }

        nvgFontFaceId(vg, l->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, icx + isz + 14.0f, row_cy, app->name, NULL);

        if (img > 0)
            nvgDeleteImage(vg, img);
    }

    if (l->result_count == 0) {
        nvgFontFaceId(vg, l->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_alpha(t->surface_text, 120));
        nvgText(vg, w / 2.0f, DC_LAUNCHER_RESULTS_Y + 60.0f, "No matching applications", NULL);
    }

    nvgEndFrame(vg);

    /* While animating, ask for a frame callback (committed by the swap) so the
     * next frame is drawn; the loop drives it via frame_done. */
    if (dc_anim_active(&l->anim) && !l->frame_cb) {
        l->frame_cb = wl_surface_frame(l->surface);
        wl_callback_add_listener(l->frame_cb, &frame_listener, l);
    }
    dc_egl_swap(l->egl, &l->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_launcher *l = data;
    DC_UNUSED(fs);
    l->scale120 = (int)scale;
    recompute_physical(l);
    launcher_render(l);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_launcher *l = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    l->logical_width = width > 0 ? (int)width : DC_LAUNCHER_WIDTH;
    l->logical_height = height > 0 ? (int)height : DC_LAUNCHER_HEIGHT;
    l->configured = true;
    recompute_physical(l);
    launcher_render(l);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_launcher *l = data;
    DC_UNUSED(surface);
    l->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_launcher *dc_launcher_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_launcher *l = calloc(1, sizeof(*l));
    l->wl = wl;
    l->egl = egl;
    l->render = render;
    l->apps = dc_apps_load();
    l->logical_width = DC_LAUNCHER_WIDTH;
    l->logical_height = DC_LAUNCHER_HEIGHT;
    l->scale120 = DC_SCALE_BASE;
    return l;
}

static void launcher_show(dc_launcher *l, dc_output *output)
{
    l->output = output;
    l->configured = false;
    l->egl_ready = false;
    l->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    l->query[0] = '\0';
    run_search(l);
    dc_anim_start(&l->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    l->surface = wl_compositor_create_surface(l->wl->compositor);
    if (l->wl->fractional_scale_mgr) {
        l->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            l->wl->fractional_scale_mgr, l->surface);
        wp_fractional_scale_v1_add_listener(l->fractional_scale, &fractional_scale_listener, l);
    }
    if (l->wl->viewporter)
        l->viewport = wp_viewporter_get_viewport(l->wl->viewporter, l->surface);

    l->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        l->wl->layer_shell, l->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:launcher");
    /* No anchors -> compositor centers the surface. */
    zwlr_layer_surface_v1_set_size(l->layer_surface, DC_LAUNCHER_WIDTH, DC_LAUNCHER_HEIGHT);
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        l->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_EXCLUSIVE);
    zwlr_layer_surface_v1_add_listener(l->layer_surface, &layer_surface_listener, l);

    wl_surface_commit(l->surface);
    l->visible = true;
    dc_debug("launcher shown");
}

static void launcher_hide(dc_launcher *l)
{
    if (l->frame_cb) {
        wl_callback_destroy(l->frame_cb);
        l->frame_cb = NULL;
    }
    if (l->egl_ready)
        dc_egl_window_finish(&l->egl_window, l->egl);
    if (l->viewport)
        wp_viewport_destroy(l->viewport);
    if (l->fractional_scale)
        wp_fractional_scale_v1_destroy(l->fractional_scale);
    if (l->layer_surface)
        zwlr_layer_surface_v1_destroy(l->layer_surface);
    if (l->surface)
        wl_surface_destroy(l->surface);
    l->egl_ready = false;
    l->configured = false;
    l->viewport = NULL;
    l->fractional_scale = NULL;
    l->layer_surface = NULL;
    l->surface = NULL;
    l->visible = false;
    dc_debug("launcher hidden");
}

void dc_launcher_toggle(dc_launcher *l, dc_output *output)
{
    if (l->visible)
        launcher_hide(l);
    else
        launcher_show(l, output);
}

void dc_launcher_hide(dc_launcher *l)
{
    if (l->visible)
        launcher_hide(l);
}

bool dc_launcher_visible(dc_launcher *l)
{
    return l->visible;
}

struct wl_surface *dc_launcher_surface(dc_launcher *l)
{
    return l->surface;
}

void dc_launcher_handle_key(dc_launcher *l, uint32_t keysym, const char *utf8)
{
    if (!l->visible)
        return;

    switch (keysym) {
    case XKB_KEY_Escape:
        launcher_hide(l);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (l->selected >= 0 && l->selected < l->result_count) {
            dc_app_launch(l->results[l->selected]);
            launcher_hide(l);
        }
        return;
    case XKB_KEY_BackSpace: {
        size_t n = strlen(l->query);
        if (n > 0) {
            l->query[n - 1] = '\0';
            run_search(l);
        }
        break;
    }
    case XKB_KEY_Up:
        l->selected--;
        clamp_scroll(l);
        break;
    case XKB_KEY_Down:
        l->selected++;
        clamp_scroll(l);
        break;
    default: {
        /* Append printable text (single-byte control chars filtered). */
        if (utf8 && utf8[0] && !((unsigned char)utf8[0] < 0x20) && (unsigned char)utf8[0] != 0x7f) {
            size_t n = strlen(l->query);
            size_t add = strlen(utf8);
            if (n + add < sizeof(l->query)) {
                memcpy(l->query + n, utf8, add + 1);
                run_search(l);
            }
        }
        break;
    }
    }
    launcher_render(l);
}

void dc_launcher_handle_click(dc_launcher *l, double x, double y)
{
    if (!l->visible)
        return;
    DC_UNUSED(x);
    if (y < DC_LAUNCHER_RESULTS_Y)
        return;
    int r = (int)((y - DC_LAUNCHER_RESULTS_Y) / DC_LAUNCHER_ROW_H);
    int idx = l->scroll + r;
    if (r >= 0 && r < DC_LAUNCHER_MAX_ROWS && idx >= 0 && idx < l->result_count) {
        dc_app_launch(l->results[idx]);
        launcher_hide(l);
    }
}

void dc_launcher_destroy(dc_launcher *l)
{
    if (!l)
        return;
    if (l->visible)
        launcher_hide(l);
    dc_apps_destroy(l->apps);
    free(l);
}
