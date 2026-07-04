#include "ui/osd.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "core/loop.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "render/shape.h"
#include "theme/theme.h"
#include "ui/bar/bar.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_OSD_WIDTH 320
#define DC_OSD_HEIGHT 60
#define DC_SCALE_BASE 120
/* Fixed bottom margin when the bar is at the TOP (no bar to clear down
 * there — just DMS's usual OSD lift off the screen edge). When the bar is
 * at the BOTTOM, the margin is computed from dc_bar_window_height() instead
 * so the OSD always clears it (docs/13-POPOUTS-SPEC.md sec.0). */
#define DC_OSD_MARGIN_NO_BAR 80
/* Extra visual gap above the bar, matching popout.c's DC_POPOUT_BAR_GAP. */
#define DC_OSD_BAR_GAP 8
/* Side inset for the bottom-left/bottom-right position variants (docs/14-
 * COMPLETION-PLAN.md W2.3), matching the bar's own screen-edge spacing. */
#define DC_OSD_SIDE_MARGIN 16

/* OSD mode. VOLUME/BRIGHTNESS are the original value+progress-bar variants;
 * the rest are icon+text-label variants added for mic mute/media/power
 * profile/output switch (see osd_render()'s mode switch below). */
typedef enum {
    DC_OSD_MODE_VOLUME,
    DC_OSD_MODE_BRIGHTNESS,
    DC_OSD_MODE_MIC_MUTE,
    DC_OSD_MODE_MEDIA,
    DC_OSD_MODE_POWER_PROFILE,
    DC_OSD_MODE_OUTPUT_SWITCH,
} dc_osd_mode;

/* True for the two original value+progress-bar variants; false for the
 * icon+text-label variants. */
static bool osd_mode_is_value(dc_osd_mode mode)
{
    return mode == DC_OSD_MODE_VOLUME || mode == DC_OSD_MODE_BRIGHTNESS;
}

struct dc_osd {
    dc_wayland *wl;
    dc_egl *egl;
    dc_render *render;
    dc_output *output;

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

    int timer_fd;

    /* Mode-specific state. */
    dc_osd_mode mode;
    int value;           /* 0-100 percent, applies to both volume and brightness */
    int icon;            /* Material Symbols codepoint to display */
    bool muted;          /* Volume only: whether muted */
    char label[96];       /* Text-label variants only (mic/media/power/output) */

    bool visible;
    bool configured;
    bool egl_ready;

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;
};

static void osd_render(dc_osd *osd);
static void osd_hide(dc_osd *osd);

static void osd_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_osd *osd = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    osd->frame_cb = NULL;
    if (!osd->visible)
        return;
    if (dc_anim_active(&osd->anim))
        osd_render(osd);
    else if (osd->closing)
        osd_hide(osd); /* exit fade finished */
}
static const struct wl_callback_listener osd_frame_listener = {.done = osd_frame_done};

static void recompute_physical(dc_osd *osd)
{
    osd->phys_width = (osd->logical_width * osd->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    osd->phys_height = (osd->logical_height * osd->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void osd_render(dc_osd *osd)
{
    if (!osd->configured || osd->phys_width <= 0)
        return;
    if (!osd->egl_ready) {
        if (!dc_egl_window_init(&osd->egl_window, osd->egl, osd->surface, osd->phys_width,
                                osd->phys_height))
            return;
        osd->egl_ready = true;
    } else {
        dc_egl_window_resize(&osd->egl_window, osd->phys_width, osd->phys_height);
    }
    if (!dc_egl_make_current(osd->egl, &osd->egl_window))
        return;
    if (!dc_render_ensure(osd->render))
        return;
    if (osd->viewport)
        wp_viewport_set_destination(osd->viewport, osd->logical_width, osd->logical_height);

    NVGcontext *vg = osd->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = osd->logical_width;
    const float h = osd->logical_height;
    const float pad = 6.0f;

    glViewport(0, 0, osd->phys_width, osd->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)osd->scale120 / DC_SCALE_BASE);

    /* Entrance/exit: fade + slide up from below (DMS OSD). Closing reverses. */
    float p = dc_anim_progress(&osd->anim);
    if (osd->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float ap = p > 1.0f ? 1.0f : p;
    nvgGlobalAlpha(vg, ap);
    nvgTranslate(vg, 0.0f, (1.0f - ap) * 16.0f);

    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 14.0f, 16.0f,
                                     nvgRGBA(0, 0, 0, 90), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 14.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 14.0f);
    nvgFillColor(vg, nvgRGBA(t->surface_container.r, t->surface_container.g,
                             t->surface_container.b, 255));
    nvgFill(vg);

    const float cy = h / 2.0f;
    dc_render_icon(osd->render, osd->icon, pad + 18.0f, cy, 22.0f, t->surface_text,
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    const float tx = pad + 52.0f;

    if (osd_mode_is_value(osd->mode)) {
        const float tw = w - pad - 52.0f - 52.0f;
        const float th = 8.0f;
        float frac = osd->value / 100.0f;
        if (frac < 0.0f)
            frac = 0.0f;
        if (frac > 1.0f)
            frac = 1.0f;

        nvgBeginPath(vg);
        nvgRoundedRect(vg, tx, cy - th / 2.0f, tw, th, th / 2.0f);
        nvgFillColor(vg, nvgRGBA(t->outline.r, t->outline.g, t->outline.b, 90));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, tx, cy - th / 2.0f, tw * frac, th, th / 2.0f);
        nvgFillColor(vg, nvgRGBA(t->primary.r, t->primary.g, t->primary.b, 255));
        nvgFill(vg);

        char pct_label[8];
        snprintf(pct_label, sizeof(pct_label), "%d", osd->value);
        nvgFontFaceId(vg, osd->render->font_ui);
        nvgFontSize(vg, 14.0f);
        nvgFillColor(vg, nvgRGBA(t->surface_text.r, t->surface_text.g, t->surface_text.b, 255));
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg, w - pad - 18.0f, cy, pct_label, NULL);
    } else {
        /* Icon+text-label variants (mic mute/media/power profile/output
         * switch): no progress bar/percent, just the label filling the rest
         * of the pill, ellipsized to fit -- same truncation convention as
         * e.g. ui/launcher.c's row labels. */
        const float lw = w - pad - tx - 12.0f;
        char buf[96];
        nvgFontFaceId(vg, osd->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgFillColor(vg, nvgRGBA(t->surface_text.r, t->surface_text.g, t->surface_text.b, 255));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        dc_shape_ellipsize(osd->render, osd->label, lw, buf, sizeof(buf));
        dc_shape_draw_text(osd->render, tx, cy, buf, NULL);
    }

    nvgEndFrame(vg);

    if ((dc_anim_active(&osd->anim) || osd->closing) && !osd->frame_cb) {
        osd->frame_cb = wl_surface_frame(osd->surface);
        wl_callback_add_listener(osd->frame_cb, &osd_frame_listener, osd);
    }
    dc_egl_swap(osd->egl, &osd->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_osd *osd = data;
    DC_UNUSED(fs);
    osd->scale120 = (int)scale;
    recompute_physical(osd);
    osd_render(osd);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_osd *osd = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    osd->logical_width = width > 0 ? (int)width : DC_OSD_WIDTH;
    osd->logical_height = height > 0 ? (int)height : DC_OSD_HEIGHT;
    osd->configured = true;
    recompute_physical(osd);
    osd_render(osd);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_osd *osd = data;
    DC_UNUSED(surface);
    osd->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

static void osd_hide(dc_osd *osd)
{
    if (!osd->visible)
        return;
    if (osd->frame_cb) {
        wl_callback_destroy(osd->frame_cb);
        osd->frame_cb = NULL;
    }
    if (osd->egl_ready)
        dc_egl_window_finish(&osd->egl_window, osd->egl);
    if (osd->viewport)
        wp_viewport_destroy(osd->viewport);
    if (osd->fractional_scale)
        wp_fractional_scale_v1_destroy(osd->fractional_scale);
    if (osd->layer_surface)
        zwlr_layer_surface_v1_destroy(osd->layer_surface);
    if (osd->surface)
        wl_surface_destroy(osd->surface);
    osd->egl_ready = false;
    osd->configured = false;
    osd->viewport = NULL;
    osd->fractional_scale = NULL;
    osd->layer_surface = NULL;
    osd->surface = NULL;
    osd->visible = false;
    osd->closing = false;
}

/* Begin the exit fade; teardown happens when it completes. */
static void osd_begin_close(dc_osd *osd)
{
    if (!osd->visible || osd->closing)
        return;
    dc_anim_start(&osd->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    osd->closing = true;
    if (!dc_anim_active(&osd->anim)) {
        osd_hide(osd);
        return;
    }
    osd_render(osd);
}

/* Auto-hide delay, config-driven (docs/14-COMPLETION-PLAN.md W2.3, settings'
 * OSD tab). Re-read from dc_config_current on every arm so a change while an
 * OSD happens to be up takes effect on its very next re-arm. */
static void arm_timer(dc_osd *osd)
{
    int ms = dc_config_current->osd_timeout_ms;
    if (ms < 200)
        ms = 200; /* guard against a corrupt/zero config value disarming the timer */
    struct itimerspec spec = {0};
    spec.it_value.tv_sec = ms / 1000;
    spec.it_value.tv_nsec = (ms % 1000) * 1000000L;
    timerfd_settime(osd->timer_fd, 0, &spec, NULL);
}

static void timer_cb(int fd, uint32_t revents, void *data)
{
    DC_UNUSED(revents);
    uint64_t expirations;
    if (read(fd, &expirations, sizeof(expirations)) < 0)
        return;
    osd_begin_close((dc_osd *)data);
}

dc_osd *dc_osd_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_osd *osd = calloc(1, sizeof(*osd));
    osd->wl = wl;
    osd->egl = egl;
    osd->render = render;
    osd->logical_width = DC_OSD_WIDTH;
    osd->logical_height = DC_OSD_HEIGHT;
    osd->scale120 = DC_SCALE_BASE;
    osd->timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    return osd;
}

void dc_osd_integrate(dc_osd *osd, struct dc_loop *loop)
{
    if (osd->timer_fd >= 0)
        dc_loop_add_fd(loop, osd->timer_fd, POLLIN, timer_cb, osd);
}

/* Shared tail of every dc_osd_show_*() call: (re)shows the surface on
 * `output` and (re)arms the auto-hide timer. Callers set osd->mode/icon/
 * value/muted/label first. Factored out of what used to be
 * osd_show_generic()'s body so the new label-mode variants (mic mute/media/
 * power profile/output switch) can share it without duplicating the layer-
 * surface/positioning/animation logic. */
static void osd_present(dc_osd *osd, dc_output *output)
{
    if (osd->visible) {
        /* A new change during the exit fade cancels it and re-shows. */
        if (osd->closing) {
            osd->closing = false;
            dc_anim_start(&osd->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_DECEL);
        }
        osd_render(osd);
        arm_timer(osd);
        return;
    }

    osd->output = output;
    osd->configured = false;
    osd->egl_ready = false;
    osd->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    dc_anim_start(&osd->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_DECEL);

    osd->surface = wl_compositor_create_surface(osd->wl->compositor);
    if (osd->wl->fractional_scale_mgr) {
        osd->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            osd->wl->fractional_scale_mgr, osd->surface);
        wp_fractional_scale_v1_add_listener(osd->fractional_scale, &fractional_scale_listener, osd);
    }
    if (osd->wl->viewporter)
        osd->viewport = wp_viewporter_get_viewport(osd->wl->viewporter, osd->surface);

    osd->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        osd->wl->layer_shell, osd->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:osd");
    zwlr_layer_surface_v1_set_size(osd->layer_surface, DC_OSD_WIDTH, DC_OSD_HEIGHT);

    /* Position (docs/14-COMPLETION-PLAN.md W2.3, config.h's osd_position):
     * bottom variants must clear a bottom bar (docs/13-POPOUTS-SPEC.md
     * sec.0); a top bar doesn't intrude on the bottom edge, so keep the
     * original fixed lift there. The top-center variant mirrors that same
     * logic against the top edge. */
    const dc_config *cfg = dc_config_current;
    int32_t bottom_lift = (cfg->bar_position == DC_BAR_POSITION_BOTTOM)
                              ? dc_bar_window_height(cfg) + DC_OSD_BAR_GAP
                              : DC_OSD_MARGIN_NO_BAR;
    int32_t top_lift = (cfg->bar_position == DC_BAR_POSITION_TOP)
                           ? dc_bar_window_height(cfg) + DC_OSD_BAR_GAP
                           : DC_OSD_MARGIN_NO_BAR;
    uint32_t anchor;
    int32_t mt = 0, mr = 0, mb = 0, ml = 0;
    switch ((dc_osd_position)cfg->osd_position) {
    case DC_OSD_POS_BOTTOM_LEFT:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT;
        mb = bottom_lift;
        ml = DC_OSD_SIDE_MARGIN;
        break;
    case DC_OSD_POS_BOTTOM_RIGHT:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT;
        mb = bottom_lift;
        mr = DC_OSD_SIDE_MARGIN;
        break;
    case DC_OSD_POS_TOP_CENTER:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP;
        mt = top_lift;
        break;
    case DC_OSD_POS_BOTTOM_CENTER:
    default:
        anchor = ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
        mb = bottom_lift;
        break;
    }
    zwlr_layer_surface_v1_set_anchor(osd->layer_surface, anchor);
    zwlr_layer_surface_v1_set_margin(osd->layer_surface, mt, mr, mb, ml);
    zwlr_layer_surface_v1_set_exclusive_zone(osd->layer_surface, -1);
    zwlr_layer_surface_v1_add_listener(osd->layer_surface, &layer_surface_listener, osd);

    wl_surface_commit(osd->surface);
    osd->visible = true;
    arm_timer(osd);
}

/* Generic OSD show: displays either volume or brightness with a custom icon. */
static void osd_show_generic(dc_osd *osd, dc_output *output, dc_osd_mode mode, int value, int icon,
                             bool muted)
{
    osd->mode = mode;
    osd->value = value;
    osd->icon = icon;
    osd->muted = muted;

    const char *mode_name = (mode == DC_OSD_MODE_VOLUME) ? "volume" : "brightness";
    dc_debug("osd %s %d%s", mode_name, value, (mode == DC_OSD_MODE_VOLUME && muted) ? " (muted)" : "");

    osd_present(osd, output);
}

/* Icon+text-label OSD show, shared by the mic mute/media/power profile/
 * output switch variants below (osd_render() draws these without the
 * progress bar + percent number, see osd_mode_is_value()). */
static void osd_show_label(dc_osd *osd, dc_output *output, dc_osd_mode mode, int icon,
                           const char *label)
{
    osd->mode = mode;
    osd->icon = icon;
    osd->muted = false;
    snprintf(osd->label, sizeof(osd->label), "%s", label ? label : "");

    dc_debug("osd mode=%d: %s", (int)mode, osd->label);

    osd_present(osd, output);
}

void dc_osd_show_volume(dc_osd *osd, dc_output *output, int volume, bool muted)
{
    int icon = muted ? DC_ICON_VOLUME_OFF : DC_ICON_VOLUME_UP;
    osd_show_generic(osd, output, DC_OSD_MODE_VOLUME, volume, icon, muted);
}

void dc_osd_show_brightness(dc_osd *osd, dc_output *output, int brightness)
{
    osd_show_generic(osd, output, DC_OSD_MODE_BRIGHTNESS, brightness, DC_ICON_BRIGHTNESS_MEDIUM, false);
}

void dc_osd_show_mic_mute(dc_osd *osd, dc_output *output, bool muted)
{
    int icon = muted ? DC_ICON_MIC_OFF : DC_ICON_MIC;
    osd_show_label(osd, output, DC_OSD_MODE_MIC_MUTE, icon, muted ? "Microphone Off" : "Microphone On");
}

void dc_osd_show_media(dc_osd *osd, dc_output *output, bool playing, const char *title)
{
    /* Icon reflects the new state itself (like VOLUME_UP/VOLUME_OFF above),
     * not the "next action" a play/pause button would perform. */
    int icon = playing ? DC_ICON_PLAY_ARROW : DC_ICON_PAUSE;
    const char *label = (title && title[0]) ? title : (playing ? "Playing" : "Paused");
    osd_show_label(osd, output, DC_OSD_MODE_MEDIA, icon, label);
}

void dc_osd_show_power_profile(dc_osd *osd, dc_output *output, const char *label)
{
    /* Best-fit icons from the existing subset (no exact "balance"/"eco"
     * glyph available -- see this task's final report for the compromise
     * this represents): TUNE (sliders) for Balanced, SPEED for Performance,
     * BATTERY_FULL for Power Saver, generic POWER otherwise. */
    int icon = DC_ICON_POWER;
    if (label) {
        if (strcmp(label, "Performance") == 0)
            icon = DC_ICON_SPEED;
        else if (strcmp(label, "Balanced") == 0)
            icon = DC_ICON_TUNE;
        else if (strcmp(label, "Power Saver") == 0)
            icon = DC_ICON_BATTERY_FULL;
    }
    osd_show_label(osd, output, DC_OSD_MODE_POWER_PROFILE, icon, label);
}

void dc_osd_show_output_switch(dc_osd *osd, dc_output *output, const char *device_name)
{
    /* HEADPHONES is the closest existing glyph to a generic "audio output
     * device" icon (already reused this way for bluetooth headset devices
     * in ui/controlcenter.c's cc_bt_device_icon()) -- no plain
     * "speaker"/"output" glyph in the current font subset. */
    osd_show_label(osd, output, DC_OSD_MODE_OUTPUT_SWITCH, DC_ICON_HEADPHONES, device_name);
}

void dc_osd_destroy(dc_osd *osd)
{
    if (!osd)
        return;
    osd_hide(osd);
    if (osd->timer_fd >= 0)
        close(osd->timer_fd);
    free(osd);
}
