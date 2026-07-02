#include "ui/settings.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/nvg.h"
#include "theme/theme.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_SET_WIDTH 520
#define DC_SET_HEIGHT 560
#define DC_SCALE_BASE 120
#define DC_SET_PAD 6.0f
#define DC_SET_INSET 20.0f
#define DC_SET_COLS 5

struct dc_settings {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    int logical_width, logical_height, scale120, phys_width, phys_height;
    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing, visible, configured, egl_ready;

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

/* Shared layout so render + hit-test agree. */
typedef struct {
    float ix, iw;
    float sw_y, sw_size, sw_gap;
    float tog_y, tog_h;
    float speed_y, track_x, track_w;
} s_layout;

static s_layout layout(float w)
{
    s_layout l;
    l.ix = DC_SET_INSET;
    l.iw = w - 2.0f * DC_SET_INSET;
    l.sw_gap = 10.0f;
    l.sw_size = (l.iw - (DC_SET_COLS - 1) * l.sw_gap) / DC_SET_COLS;
    l.sw_y = 78.0f;
    float sw_rows_h = 2.0f * (l.sw_size * 0.55f) + l.sw_gap; /* swatches are shorter than wide */
    l.tog_y = l.sw_y + sw_rows_h + 34.0f;
    l.tog_h = 44.0f;
    l.speed_y = l.tog_y + 4 * l.tog_h + 28.0f;
    l.track_x = l.ix + 150.0f;
    l.track_w = l.iw - 150.0f;
    return l;
}

static const char *const TOGGLE_LABELS[4] = {"24-hour clock", "Show date", "Animations",
                                             "Dynamic color"};

static bool toggle_value(const dc_config *c, int i)
{
    switch (i) {
    case 0:
        return c->clock_24h;
    case 1:
        return c->show_date;
    case 2:
        return c->animations_enabled;
    case 3:
        return c->dynamic_color;
    }
    return false;
}

static void recompute_physical(dc_settings *s)
{
    s->phys_width = (s->logical_width * s->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    s->phys_height = (s->logical_height * s->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void s_render(dc_settings *s);
static void s_teardown(dc_settings *s);

static void s_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_settings *s = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    s->frame_cb = NULL;
    if (!s->visible)
        return;
    if (dc_anim_active(&s->anim))
        s_render(s);
    else if (s->closing)
        s_teardown(s);
}
static const struct wl_callback_listener s_frame_listener = {.done = s_frame_done};

/* A toggle pill (green on / outline off). */
static void draw_toggle(dc_render *r, float x, float cy, bool on)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    const float pw = 44.0f, ph = 24.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, cy - ph / 2.0f, pw, ph, ph / 2.0f);
    nvgFillColor(vg, on ? tc(t->primary) : tc_alpha(t->outline, 120));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, on ? x + pw - ph / 2.0f : x + ph / 2.0f, cy, ph / 2.0f - 3.0f);
    nvgFillColor(vg, on ? tc(t->primary_text) : tc(t->surface_text));
    nvgFill(vg);
}

static void s_render(dc_settings *s)
{
    if (!s->configured || s->phys_width <= 0)
        return;
    if (!s->egl_ready) {
        if (!dc_egl_window_init(&s->egl_window, s->egl, s->surface, s->phys_width, s->phys_height))
            return;
        s->egl_ready = true;
    } else {
        dc_egl_window_resize(&s->egl_window, s->phys_width, s->phys_height);
    }
    if (!dc_egl_make_current(s->egl, &s->egl_window))
        return;
    if (!dc_render_ensure(s->render))
        return;
    if (s->viewport)
        wp_viewport_set_destination(s->viewport, s->logical_width, s->logical_height);

    NVGcontext *vg = s->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    const float w = s->logical_width, h = s->logical_height, pad = DC_SET_PAD;
    s_layout l = layout(w);

    glViewport(0, 0, s->phys_width, s->phys_height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)s->scale120 / DC_SCALE_BASE);

    float p = dc_anim_progress(&s->anim);
    if (s->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.94f + 0.06f * p;
    float ox = pad + (w - 2.0f * pad) * s->anim_ox;
    float oy = pad + (h - 2.0f * pad) * s->anim_oy;
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

    nvgFontFaceId(vg, s->render->font_ui);
    nvgFontSize(vg, 18.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, l.ix, pad + 28.0f, "Settings", NULL);

    nvgFontSize(vg, 13.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, l.ix, l.sw_y - 16.0f, "Theme", NULL);

    /* Theme swatches. */
    int count = dc_theme_count();
    const float sh = l.sw_size * 0.55f;
    for (int i = 0; i < count; i++) {
        int col = i % DC_SET_COLS, row = i / DC_SET_COLS;
        float x = l.ix + col * (l.sw_size + l.sw_gap);
        float y = l.sw_y + row * (sh + l.sw_gap);
        bool cur = strcmp(cfg->theme_id, dc_theme_id_at(i)) == 0;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, x, y, l.sw_size, sh, 10.0f);
        nvgFillColor(vg, tc(dc_theme_primary_at(i)));
        nvgFill(vg);
        if (cur) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x - 2.0f, y - 2.0f, l.sw_size + 4.0f, sh + 4.0f, 12.0f);
            nvgStrokeColor(vg, tc(t->surface_text));
            nvgStrokeWidth(vg, 2.5f);
            nvgStroke(vg);
        }
    }

    /* Toggle rows. */
    for (int i = 0; i < 4; i++) {
        float cy = l.tog_y + i * l.tog_h + l.tog_h / 2.0f;
        nvgFontSize(vg, 15.0f);
        nvgFillColor(vg, tc(t->surface_text));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, l.ix, cy, TOGGLE_LABELS[i], NULL);
        draw_toggle(s->render, l.ix + l.iw - 44.0f, cy, toggle_value(cfg, i));
    }

    /* Animation-speed slider. */
    float speed = (cfg->animation_speed - 0.25f) / 3.75f;
    if (speed < 0.0f)
        speed = 0.0f;
    if (speed > 1.0f)
        speed = 1.0f;
    float scy = l.speed_y + 12.0f;
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, l.ix, scy, "Anim speed", NULL);
    const float th = 8.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l.track_x, scy - th / 2.0f, l.track_w, th, th / 2.0f);
    nvgFillColor(vg, tc_alpha(t->outline, 90));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l.track_x, scy - th / 2.0f, l.track_w * speed, th, th / 2.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);
    nvgBeginPath(vg);
    nvgCircle(vg, l.track_x + l.track_w * speed, scy, 8.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);

    nvgEndFrame(vg);
    if ((dc_anim_active(&s->anim) || s->closing) && !s->frame_cb) {
        s->frame_cb = wl_surface_frame(s->surface);
        wl_callback_add_listener(s->frame_cb, &s_frame_listener, s);
    }
    dc_egl_swap(s->egl, &s->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_settings *s = data;
    DC_UNUSED(fs);
    s->scale120 = (int)scale;
    recompute_physical(s);
    s_render(s);
}
static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_settings *s = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    s->logical_width = width > 0 ? (int)width : DC_SET_WIDTH;
    s->logical_height = height > 0 ? (int)height : DC_SET_HEIGHT;
    s->configured = true;
    recompute_physical(s);
    s_render(s);
}
static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_settings *s = data;
    DC_UNUSED(surface);
    s->configured = false;
}
static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_settings *dc_settings_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_settings *s = calloc(1, sizeof(*s));
    s->wl = wl;
    s->egl = egl;
    s->render = render;
    s->logical_width = DC_SET_WIDTH;
    s->logical_height = DC_SET_HEIGHT;
    s->scale120 = DC_SCALE_BASE;
    return s;
}

static void s_show(dc_settings *s, dc_output *output)
{
    s->output = output;
    s->configured = false;
    s->egl_ready = false;
    s->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    dc_anim_start(&s->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    s->surface = wl_compositor_create_surface(s->wl->compositor);
    if (s->wl->fractional_scale_mgr) {
        s->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            s->wl->fractional_scale_mgr, s->surface);
        wp_fractional_scale_v1_add_listener(s->fractional_scale, &fractional_scale_listener, s);
    }
    if (s->wl->viewporter)
        s->viewport = wp_viewporter_get_viewport(s->wl->viewporter, s->surface);

    s->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        s->wl->layer_shell, s->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:settings");

    /* Bar-adjacent, horizontally centered (docs/13-POPOUTS-SPEC.md sec.0). */
    dc_popout_anchor pa = dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_CENTER, 0);
    s->anim_ox = pa.origin_x;
    s->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(s->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(s->layer_surface, DC_SET_WIDTH, DC_SET_HEIGHT);
    zwlr_layer_surface_v1_set_margin(s->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_add_listener(s->layer_surface, &layer_surface_listener, s);
    wl_surface_commit(s->surface);
    s->visible = true;
    s->closing = false;
    dc_debug("settings shown");
}

static void s_teardown(dc_settings *s)
{
    if (s->frame_cb) {
        wl_callback_destroy(s->frame_cb);
        s->frame_cb = NULL;
    }
    if (s->egl_ready)
        dc_egl_window_finish(&s->egl_window, s->egl);
    if (s->viewport)
        wp_viewport_destroy(s->viewport);
    if (s->fractional_scale)
        wp_fractional_scale_v1_destroy(s->fractional_scale);
    if (s->layer_surface)
        zwlr_layer_surface_v1_destroy(s->layer_surface);
    if (s->surface)
        wl_surface_destroy(s->surface);
    s->egl_ready = false;
    s->configured = false;
    s->viewport = NULL;
    s->fractional_scale = NULL;
    s->layer_surface = NULL;
    s->surface = NULL;
    s->visible = false;
    s->closing = false;
    dc_debug("settings hidden");
}

static void s_begin_close(dc_settings *s)
{
    if (!s->visible || s->closing)
        return;
    dc_anim_start(&s->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    s->closing = true;
    if (!dc_anim_active(&s->anim)) {
        s_teardown(s);
        return;
    }
    s_render(s);
}

void dc_settings_toggle(dc_settings *s, dc_output *output)
{
    if (s->visible)
        s_begin_close(s);
    else
        s_show(s, output);
}

void dc_settings_hide(dc_settings *s)
{
    s_begin_close(s);
}

bool dc_settings_visible(dc_settings *s)
{
    return s->visible;
}

struct wl_surface *dc_settings_surface(dc_settings *s)
{
    return s->surface;
}

void dc_settings_handle_click(dc_settings *s, double x, double y)
{
    if (!s->visible || s->closing)
        return;
    s_layout l = layout((float)s->logical_width);
    dc_config *cfg = dc_config_mut();
    const float sh = l.sw_size * 0.55f;

    /* Theme swatch? */
    int count = dc_theme_count();
    for (int i = 0; i < count; i++) {
        int col = i % DC_SET_COLS, row = i / DC_SET_COLS;
        float sx = l.ix + col * (l.sw_size + l.sw_gap);
        float sy = l.sw_y + row * (sh + l.sw_gap);
        if (x >= sx && x <= sx + l.sw_size && y >= sy && y <= sy + sh) {
            snprintf(cfg->theme_id, sizeof(cfg->theme_id), "%s", dc_theme_id_at(i));
            dc_config_reapply();
            dc_config_save();
            s_render(s);
            return;
        }
    }

    /* Toggle row? */
    for (int i = 0; i < 4; i++) {
        float ty = l.tog_y + i * l.tog_h;
        if (y >= ty && y <= ty + l.tog_h) {
            bool v = !toggle_value(cfg, i);
            switch (i) {
            case 0:
                cfg->clock_24h = v;
                break;
            case 1:
                cfg->show_date = v;
                break;
            case 2:
                cfg->animations_enabled = v;
                break;
            case 3:
                cfg->dynamic_color = v;
                dc_config_reapply();
                break;
            }
            dc_config_save();
            s_render(s);
            return;
        }
    }

    /* Animation-speed slider? */
    float scy = l.speed_y + 12.0f;
    if (y >= scy - 16.0f && y <= scy + 16.0f && x >= l.track_x - 8.0f &&
        x <= l.track_x + l.track_w + 8.0f) {
        float frac = (float)(x - l.track_x) / l.track_w;
        if (frac < 0.0f)
            frac = 0.0f;
        if (frac > 1.0f)
            frac = 1.0f;
        cfg->animation_speed = 0.25f + frac * 3.75f;
        dc_config_save();
        s_render(s);
    }
}

void dc_settings_destroy(dc_settings *s)
{
    if (!s)
        return;
    if (s->visible)
        s_teardown(s);
    free(s);
}
