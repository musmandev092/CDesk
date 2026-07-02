#include "ui/bar/bar.h"

#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "niri/niri.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/audio.h"
#include "services/battery.h"
#include "services/bluez.h"
#include "services/icons.h"
#include "services/mpris.h"
#include "services/net.h"
#include "services/tray.h"
#include "theme/theme.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <ctype.h>
#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Fallback logical height before the first layer-surface configure lands
 * (docs/12-BAR-SPEC.md sec.1 overrides this via dc_bar_geometry()). */
#define DC_BAR_HEIGHT_FALLBACK 48

/* Corner radius of the floating bar rect (docs/12-BAR-SPEC.md sec.1). */
#define DC_BAR_CORNER_RADIUS 12.0f

/* Fractional-scale numerator base: preferred_scale is reported over 120. */
#define DC_SCALE_BASE 120

/* Bar container geometry, derived from config (docs/12-BAR-SPEC.md sec.1):
 *   widgetThickness       = max(20, 26 + innerPadding*0.6)
 *   effectiveBarThickness = max(widgetThickness + innerPadding + 4,
 *                                48 - 4 - (8 - innerPadding))
 *   windowHeight           = effectiveBarThickness + spacing (layer-surface
 *                             size + exclusive zone; the extra `spacing` px
 *                             is the transparent gap toward the outer edge)
 */
typedef struct {
    float widget_thickness;
    float effective_thickness;
    int window_height;
} dc_bar_geometry;

static dc_bar_geometry bar_compute_geometry(const dc_config *cfg)
{
    float ip = (float)cfg->bar_inner_padding;
    dc_bar_geometry g;
    g.widget_thickness = fmaxf(20.0f, 26.0f + ip * 0.6f);
    g.effective_thickness = fmaxf(g.widget_thickness + ip + 4.0f, 48.0f - 4.0f - (8.0f - ip));
    g.window_height = (int)lroundf(g.effective_thickness) + cfg->bar_spacing;
    return g;
}

/* dc_color -> nanovg color. */
static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline NVGcolor tc_alpha(dc_color c, int alpha)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)alpha);
}

struct dc_bar {
    dc_wayland *wl;
    dc_output *output;
    dc_egl *egl;
    dc_render *render;
    dc_niri *niri;

    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_fractional_scale_v1 *fractional_scale;
    struct wp_viewport *viewport;
    dc_egl_window egl_window;

    /* Focused-app icon cache (avoids re-decoding every frame). */
    char icon_app_id[64];
    int icon_image; /* nanovg image handle, 0 = none */

    /* Center x of the notification-bell + clipboard icons (dynamic; hit-test). */
    float notif_cx;
    float clip_cx;

    /* System tray (StatusNotifier) + a per-name nanovg image cache. */
    struct dc_tray *tray;
    struct {
        char name[DC_TRAY_STR];
        int image;
    } tray_cache[DC_TRAY_MAX];
    int tray_cache_n;

    int logical_width;  /* from the layer-surface configure */
    int logical_height;
    int scale120; /* fractional scale numerator (120 == 1.0x) */
    int phys_width; /* buffer size = logical * scale120 / 120 */
    int phys_height;

    /* The floating bar rect within the (larger, mostly transparent)
     * layer-surface buffer — see bar_compute_geometry() and
     * recompute_content_rect(). Widgets draw relative to this rect, not the
     * full surface. */
    float rect_x, rect_y, rect_w, rect_h;

    bool configured;
    bool egl_ready;
};

static void recompute_physical(dc_bar *bar)
{
    bar->phys_width = (bar->logical_width * bar->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    bar->phys_height = (bar->logical_height * bar->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Position the floating rect inside the surface: spacing-px gap on the outer
 * (screen) edge, flush against the inner (desktop-facing) edge, spacing-px
 * inset from the left/right sides. Call whenever logical size changes. */
static void recompute_content_rect(dc_bar *bar)
{
    const dc_config *cfg = dc_config_current;
    dc_bar_geometry geo = bar_compute_geometry(cfg);
    float spacing = (float)cfg->bar_spacing;

    bar->rect_x = spacing;
    bar->rect_w = (float)bar->logical_width - 2.0f * spacing;
    bar->rect_h = geo.effective_thickness;
    bar->rect_y = (cfg->bar_position == DC_BAR_POSITION_TOP) ? spacing : 0.0f;
}

/* Vertical center of the visible rect (widgets are y-centered in it). */
static inline float bar_cy(const dc_bar *bar)
{
    return bar->rect_y + bar->rect_h / 2.0f;
}

/* Left/right content edges: spec content inset = max(4, innerPadding*0.8). */
static inline float bar_content_inset(void)
{
    const dc_config *cfg = dc_config_current;
    return fmaxf(4.0f, (float)cfg->bar_inner_padding * 0.8f);
}

static inline float bar_content_x0(const dc_bar *bar)
{
    return bar->rect_x + bar_content_inset();
}

static inline float bar_content_x1(const dc_bar *bar)
{
    return bar->rect_x + bar->rect_w - bar_content_inset();
}

/* Apps-grid launcher icon at the far left (like DMS). Returns the x past it. */
static float draw_launcher(dc_bar *bar)
{
    const float pad = bar_content_x0(bar) + 12.0f;
    const float size = 22.0f;
    dc_render_icon(bar->render, DC_ICON_APPS, pad, bar_cy(bar), size,
                   dc_theme_current->surface_text, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    return pad + size + 10.0f;
}

/* Workspace pills, filtered to this bar's output, starting at `start_x`.
 * Returns the x coordinate just past the last pill. */
static float draw_workspaces(dc_bar *bar, float start_x)
{
    if (!bar->niri)
        return start_x;

    int count = 0;
    const dc_niri_workspace *workspaces = dc_niri_workspaces(bar->niri, &count);
    if (!workspaces)
        return start_x;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const float cy = bar_cy(bar);
    const float gap = 8.0f;
    const float dot = 9.0f;         /* inactive workspace dot diameter */
    const float pill_w = 30.0f;     /* focused workspace pill */
    const float pill_h = 11.0f;
    float x = start_x;

    /* DMS style: focused = wide primary pill, others = dots (brighter if the
     * active/visible workspace on their output). */
    for (int i = 0; i < count; i++) {
        const dc_niri_workspace *ws = &workspaces[i];
        if (bar->output->name && ws->output[0] && strcmp(ws->output, bar->output->name) != 0)
            continue;

        if (ws->is_focused) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, cy - pill_h / 2.0f, pill_w, pill_h, pill_h / 2.0f);
            nvgFillColor(vg, tc(ws->is_urgent ? t->error : t->primary));
            nvgFill(vg);
            x += pill_w + gap;
        } else {
            nvgBeginPath(vg);
            nvgCircle(vg, x + dot / 2.0f, cy, dot / 2.0f);
            if (ws->is_urgent)
                nvgFillColor(vg, tc(t->error));
            else if (ws->is_active)
                nvgFillColor(vg, tc(t->surface_variant_text));
            else
                nvgFillColor(vg, tc_alpha(t->outline, 150));
            nvgFill(vg);
            x += dot + gap;
        }
    }
    return x;
}

/* True if the focused window lives on this bar's output. */
static bool focused_window_on_output(dc_bar *bar, const dc_niri_window *win)
{
    if (!bar->output->name)
        return true; /* no connector name to filter by */

    int count = 0;
    const dc_niri_workspace *workspaces = dc_niri_workspaces(bar->niri, &count);
    for (int i = 0; i < count; i++) {
        if (workspaces[i].id != win->workspace_id)
            continue;
        return workspaces[i].output[0] == '\0' ||
               strcmp(workspaces[i].output, bar->output->name) == 0;
    }
    return false;
}

/* Focused-window title, drawn after the workspaces and clipped so it never
 * collides with the centered clock. */
static void draw_focused_window(dc_bar *bar, float start_x)
{
    const dc_niri_window *win = dc_niri_focused_window(bar->niri);
    if (!win || !focused_window_on_output(bar, win))
        return;

    /* Pretty app name from the app_id (last dotted component, capitalised),
     * then "AppName · Title" like DMS. */
    char app_name[64] = {0};
    if (win->app_id[0]) {
        const char *base = strrchr(win->app_id, '.');
        base = base ? base + 1 : win->app_id;
        snprintf(app_name, sizeof(app_name), "%s", base);
        app_name[0] = (char)toupper((unsigned char)app_name[0]);
    }

    char label[DC_NIRI_TITLE_MAX + 96];
    if (app_name[0] && win->title[0])
        snprintf(label, sizeof(label), "%s \xc2\xb7 %s", app_name, win->title);
    else if (win->title[0])
        snprintf(label, sizeof(label), "%s", win->title);
    else
        snprintf(label, sizeof(label), "%s", app_name);
    if (!label[0])
        return;

    NVGcontext *vg = bar->render->vg;
    const float cy = bar_cy(bar);
    float x = start_x + 8.0f;

    /* Refresh the cached app icon only when the focused app changes. */
    if (strcmp(win->app_id, bar->icon_app_id) != 0) {
        if (bar->icon_image > 0)
            nvgDeleteImage(vg, bar->icon_image);
        bar->icon_image = 0;
        char *path = dc_icon_resolve(win->app_id, 24, 1);
        if (path) {
            bar->icon_image = dc_render_load_icon(bar->render, path, 22);
            free(path);
        }
        snprintf(bar->icon_app_id, sizeof(bar->icon_app_id), "%s", win->app_id);
    }

    if (bar->icon_image > 0) {
        const float isz = 20.0f;
        NVGpaint pat =
            nvgImagePattern(vg, x, cy - isz / 2.0f, isz, isz, 0.0f, bar->icon_image, 1.0f);
        nvgBeginPath(vg);
        nvgRect(vg, x, cy - isz / 2.0f, isz, isz);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
        x += isz + 8.0f;
    }

    const float max_x = bar->logical_width / 2.0f - 48.0f; /* keep clear of the clock */
    const float avail = max_x - x;
    if (avail < 40.0f)
        return;

    nvgSave(vg);
    nvgScissor(vg, x, bar->rect_y, avail, bar->rect_h);
    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, tc(dc_theme_current->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x, cy, label, NULL);
    nvgRestore(vg);
}

/* Center cluster: "HH:MM  Www D", matching DMS (weather is a separate widget,
 * added once the weather service lands). Honours use24HourClock. Returns the x
 * of the group's left edge (so the media widget can sit to its left). */
static float draw_clock(dc_bar *bar)
{
    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const float cy = bar_cy(bar);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);

    const dc_config *cfg = dc_config_current;
    char time_str[16];
    char date_str[32];
    strftime(time_str, sizeof(time_str), cfg->clock_24h ? "%H:%M" : "%-I:%M %p", &tm);
    strftime(date_str, sizeof(date_str), "%a %-d", &tm); /* e.g. "Wed 1" */

    const float gap = 10.0f;
    float bounds[4];

    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgTextBounds(vg, 0.0f, 0.0f, time_str, NULL, bounds);
    const float time_w = bounds[2] - bounds[0];
    float date_w = 0.0f;
    if (cfg->show_date) {
        nvgFontSize(vg, 13.0f);
        nvgTextBounds(vg, 0.0f, 0.0f, date_str, NULL, bounds);
        date_w = bounds[2] - bounds[0];
    }

    const float total = cfg->show_date ? time_w + gap + date_w : time_w;
    const float x = bar->logical_width / 2.0f - total / 2.0f;

    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x, cy, time_str, NULL);
    if (cfg->show_date) {
        nvgFontSize(vg, 13.0f);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgText(vg, x + time_w + gap, cy, date_str, NULL);
    }
    return x;
}

/* Now-playing media (music note + title), shown left of the clock only while a
 * player is actually playing — like DMS. */
static void draw_media(dc_bar *bar, float right_x)
{
    dc_mpris_info media;
    if (!dc_mpris_read(&media) || !media.playing || !media.title[0])
        return;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const float cy = bar_cy(bar);

    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, 13.0f);
    float bounds[4];
    nvgTextBounds(vg, 0.0f, 0.0f, media.title, NULL, bounds);
    float title_w = bounds[2] - bounds[0];
    const float max_w = 240.0f;
    if (title_w > max_w)
        title_w = max_w;
    const float title_x = right_x - title_w;

    nvgSave(vg);
    nvgScissor(vg, title_x, bar->rect_y, title_w, bar->rect_h);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, title_x, cy, media.title, NULL);
    nvgRestore(vg);

    dc_render_icon(bar->render, DC_ICON_MUSIC_NOTE, title_x - 8.0f, cy, 17.0f, t->primary,
                   NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
}

/* Battery indicator (vector pictograph + percentage) ending at `right_edge`.
 * Returns the x coordinate of its left edge. */
static float draw_battery(dc_bar *bar, float right_edge)
{
    dc_battery_info bat;
    if (!dc_battery_read(&bat) || !bat.present)
        return right_edge;

    NVGcontext *vg = bar->render->vg;
    const dc_theme *t = dc_theme_current;
    const float cy = bar_cy(bar);
    const bool low = bat.percent <= 20 && !bat.charging;

    const NVGcolor text_color = low ? tc(t->error) : tc(t->surface_text);
    const NVGcolor fill_color = bat.charging ? tc(t->success) : text_color;
    const NVGcolor outline = tc_alpha(t->outline, 180);

    char label[8];
    snprintf(label, sizeof(label), "%d%%", bat.percent);

    nvgFontFaceId(vg, bar->render->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, text_color);
    const float text_right = right_edge;
    nvgText(vg, text_right, cy, label, NULL);

    float bounds[4];
    nvgTextBounds(vg, text_right, cy, label, NULL, bounds);
    const float text_width = bounds[2] - bounds[0];

    /* Pictograph: rounded body + terminal nub + proportional fill. */
    const float body_w = 22.0f, body_h = 11.0f, nub_w = 2.0f, nub_h = 5.0f, inset = 2.0f;
    const float bx = text_right - text_width - 8.0f - body_w;
    const float by = cy - body_h / 2.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, bx, by, body_w, body_h, 2.5f);
    nvgStrokeColor(vg, outline);
    nvgStrokeWidth(vg, 1.5f);
    nvgStroke(vg);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, bx + body_w, cy - nub_h / 2.0f, nub_w, nub_h, 1.0f);
    nvgFillColor(vg, outline);
    nvgFill(vg);

    const float fill_w = (body_w - 2.0f * inset) * (bat.percent / 100.0f);
    if (fill_w > 0.5f) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx + inset, by + inset, fill_w, body_h - 2.0f * inset, 1.0f);
        nvgFillColor(vg, fill_color);
        nvgFill(vg);
    }
    return bx;
}

/* Full right status cluster in DMS order (right->left): volume, bluetooth,
 * wifi, battery+%, notifications, clipboard, signal. State is static until the
 * sd-bus services (M3) drive it; colours follow DMS (wifi/signal green,
 * bluetooth info-blue, battery green, rest surfaceText). */
/* Resolve+load a tray icon by name, cached per name (nanovg image handle). */
static int get_tray_image(dc_bar *bar, const char *name)
{
    for (int i = 0; i < bar->tray_cache_n; i++)
        if (strcmp(bar->tray_cache[i].name, name) == 0)
            return bar->tray_cache[i].image;

    int img = 0;
    char *path = dc_icon_resolve(name, 20, 1);
    if (path) {
        img = dc_render_load_icon(bar->render, path, 20);
        free(path);
    }
    if (bar->tray_cache_n < DC_TRAY_MAX) {
        snprintf(bar->tray_cache[bar->tray_cache_n].name,
                 sizeof(bar->tray_cache[bar->tray_cache_n].name), "%s", name);
        bar->tray_cache[bar->tray_cache_n].image = img;
        bar->tray_cache_n++;
    }
    return img;
}

/* Tray items, drawn to the left of the signal icon (x decreasing). */
static void draw_tray(dc_bar *bar, float *x, float cy, float isize, float step)
{
    if (!bar->tray)
        return;
    const dc_tray_item *items[DC_TRAY_MAX];
    int n = dc_tray_items(bar->tray, items, DC_TRAY_MAX);
    NVGcontext *vg = bar->render->vg;
    for (int i = 0; i < n; i++) {
        *x -= step;
        int img = items[i]->icon_name[0] ? get_tray_image(bar, items[i]->icon_name) : 0;
        if (img > 0) {
            NVGpaint pat = nvgImagePattern(vg, *x - isize, cy - isize / 2.0f, isize, isize, 0.0f,
                                           img, 1.0f);
            nvgBeginPath(vg);
            nvgRect(vg, *x - isize, cy - isize / 2.0f, isize, isize);
            nvgFillPaint(vg, pat);
            nvgFill(vg);
        } else {
            /* Fallback: a small dot for pixmap-only / unresolved items. */
            nvgBeginPath(vg);
            nvgCircle(vg, *x - isize / 2.0f, cy, 4.0f);
            nvgFillColor(vg, nvgRGBA(dc_theme_current->surface_text.r,
                                     dc_theme_current->surface_text.g,
                                     dc_theme_current->surface_text.b, 200));
            nvgFill(vg);
        }
    }
}

static void draw_right_cluster(dc_bar *bar)
{
    const dc_theme *t = dc_theme_current;
    const float cy = bar_cy(bar);
    const float pad = 12.0f;
    const float isize = 19.0f;
    const float step = 26.0f;
    const int align = NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE;

    float x = bar_content_x1(bar) - pad;

    /* Volume — live level + mute state from wpctl. */
    dc_audio_info audio;
    bool have_audio = dc_audio_read(&audio);
    int volume_icon = DC_ICON_VOLUME_UP;
    if (have_audio && audio.muted)
        volume_icon = DC_ICON_VOLUME_OFF;
    else if (have_audio && audio.volume < 34)
        volume_icon = DC_ICON_VOLUME_DOWN;
    dc_color volume_color = (have_audio && audio.muted) ? t->outline : t->surface_text;
    dc_render_icon(bar->render, volume_icon, x, cy, isize, volume_color, align);
    x -= step;

    /* Bluetooth — info-blue when a device is connected, mid when powered, dim off. */
    dc_bluez_info bt;
    bool have_bt = dc_bluez_read(&bt);
    dc_color bt_color = t->outline;
    if (have_bt && bt.connected)
        bt_color = t->info;
    else if (have_bt && bt.powered)
        bt_color = t->surface_variant_text;
    dc_render_icon(bar->render, DC_ICON_BLUETOOTH, x, cy, isize, bt_color, align);
    x -= step;

    /* Wi-Fi — green when connected (sysfs), dim otherwise. */
    dc_net_info net;
    dc_net_wifi(&net);
    int wifi_icon = net.connected ? DC_ICON_WIFI : DC_ICON_NETWORK_WIFI;
    dc_color wifi_color = net.connected ? t->primary : t->outline;
    dc_render_icon(bar->render, wifi_icon, x, cy, isize, wifi_color, align);
    x -= step;

    x = draw_battery(bar, x - 4.0f) - 10.0f;

    dc_render_icon(bar->render, DC_ICON_NOTIFICATIONS, x, cy, isize, t->surface_text, align);
    bar->notif_cx = x - isize / 2.0f; /* right-aligned icon spans [x-isize, x] */
    x -= step;
    dc_render_icon(bar->render, DC_ICON_CONTENT_PASTE, x, cy, isize, t->surface_text, align);
    bar->clip_cx = x - isize / 2.0f;
    x -= step;
    dc_render_icon(bar->render, DC_ICON_SIGNAL_CELLULAR_ALT, x, cy, isize, t->primary, align);

    draw_tray(bar, &x, cy, isize, step);
}

/* Elevation level-2 shadow under the floating rect: a tight "key" shadow
 * offset toward the outer screen edge, plus a soft wide "ambient" shadow
 * (docs/12-BAR-SPEC.md sec.1). Drawn as a box-gradient with the rect punched
 * out as a hole so it never paints over the (possibly translucent) bg fill. */
static void draw_bar_shadow(dc_bar *bar)
{
    NVGcontext *vg = bar->render->vg;
    const dc_config *cfg = dc_config_current;
    const float outer_dir = (cfg->bar_position == DC_BAR_POSITION_TOP) ? -1.0f : 1.0f;

    struct {
        float blur, offset, alpha;
    } layers[2] = {
        {14.0f, 0.0f, 0.125f},        /* ambient */
        {8.0f, 4.0f * outer_dir, 0.25f}, /* key */
    };

    for (int i = 0; i < 2; i++) {
        float blur = layers[i].blur;
        float oy = bar->rect_y + layers[i].offset;
        unsigned char a = (unsigned char)(layers[i].alpha * 255.0f);

        NVGpaint paint = nvgBoxGradient(vg, bar->rect_x, oy, bar->rect_w, bar->rect_h,
                                        DC_BAR_CORNER_RADIUS, blur, nvgRGBA(0, 0, 0, a),
                                        nvgRGBA(0, 0, 0, 0));
        nvgBeginPath(vg);
        nvgRect(vg, bar->rect_x - blur, oy - blur, bar->rect_w + 2.0f * blur,
               bar->rect_h + 2.0f * blur);
        nvgRoundedRect(vg, bar->rect_x, bar->rect_y, bar->rect_w, bar->rect_h,
                       DC_BAR_CORNER_RADIUS);
        nvgPathWinding(vg, NVG_HOLE);
        nvgFillPaint(vg, paint);
        nvgFill(vg);
    }
}

/* The floating rounded rect itself: surfaceContainer x barTransparency. */
static void draw_bar_background(dc_bar *bar)
{
    NVGcontext *vg = bar->render->vg;
    const dc_config *cfg = dc_config_current;
    dc_color c = dc_theme_current->surface_container;
    unsigned char a = (unsigned char)((c.a / 255.0f) * cfg->bar_transparency * 255.0f);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, bar->rect_x, bar->rect_y, bar->rect_w, bar->rect_h, DC_BAR_CORNER_RADIUS);
    nvgFillColor(vg, nvgRGBA(c.r, c.g, c.b, a));
    nvgFill(vg);
}

void dc_bar_render(dc_bar *bar)
{
    if (!bar->configured || bar->phys_width <= 0)
        return;

    if (!bar->egl_ready) {
        if (!dc_egl_window_init(&bar->egl_window, bar->egl, bar->surface, bar->phys_width,
                                bar->phys_height))
            return;
        bar->egl_ready = true;
    } else {
        dc_egl_window_resize(&bar->egl_window, bar->phys_width, bar->phys_height);
    }

    if (!dc_egl_make_current(bar->egl, &bar->egl_window))
        return;
    if (!dc_render_ensure(bar->render))
        return;

    /* Map the large (physical) buffer down to the logical surface size. */
    if (bar->viewport)
        wp_viewport_set_destination(bar->viewport, bar->logical_width, bar->logical_height);

    /* Fully transparent clear: the bar is a floating rect, not an
     * edge-to-edge fill, so most of the buffer must stay see-through. */
    glViewport(0, 0, bar->phys_width, bar->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    float pixel_ratio = (float)bar->scale120 / DC_SCALE_BASE;
    nvgBeginFrame(bar->render->vg, bar->logical_width, bar->logical_height, pixel_ratio);
    draw_bar_shadow(bar);
    draw_bar_background(bar);
    float launcher_end = draw_launcher(bar);
    float workspaces_end = draw_workspaces(bar, launcher_end);
    draw_focused_window(bar, workspaces_end);
    float clock_left = draw_clock(bar);
    draw_media(bar, clock_left - 16.0f);
    draw_right_cluster(bar);
    nvgEndFrame(bar->render->vg);

    dc_egl_swap(bar->egl, &bar->egl_window);
}

/* --- fractional scale --------------------------------------------------- */

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_bar *bar = data;
    DC_UNUSED(fs);
    if ((int)scale == bar->scale120)
        return;
    bar->scale120 = (int)scale;
    dc_debug("bar scale on %s: %.3gx", bar->output->name ? bar->output->name : "?",
             (double)scale / DC_SCALE_BASE);
    recompute_physical(bar);
    dc_bar_render(bar);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

/* --- layer surface ------------------------------------------------------ */

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_bar *bar = data;

    /* Protocol: ack before attaching a buffer. */
    zwlr_layer_surface_v1_ack_configure(surface, serial);

    bar->logical_width = width > 0 ? (int)width : bar->logical_width;
    bar->logical_height = height > 0 ? (int)height : bar->logical_height;
    bar->configured = true;
    recompute_physical(bar);
    recompute_content_rect(bar);
    dc_debug("bar configured: %dx%d logical (buffer %dx%d), rect %.0fx%.0f+%.0f+%.0f on %s",
             bar->logical_width, bar->logical_height, bar->phys_width, bar->phys_height,
             bar->rect_w, bar->rect_h, bar->rect_x, bar->rect_y,
             bar->output->model ? bar->output->model : "?");
    dc_bar_render(bar);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_bar *bar = data;
    DC_UNUSED(surface);
    dc_debug("bar surface closed by compositor");
    bar->configured = false;
    bar->egl_ready = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_bar *dc_bar_create(dc_wayland *wl, dc_output *output, dc_egl *egl, dc_render *render,
                      dc_niri *niri)
{
    const dc_config *cfg = dc_config_current;
    dc_bar_geometry geo = bar_compute_geometry(cfg);

    dc_bar *bar = calloc(1, sizeof(*bar));
    bar->wl = wl;
    bar->output = output;
    bar->egl = egl;
    bar->render = render;
    bar->niri = niri;
    bar->logical_height = geo.window_height > 0 ? geo.window_height : DC_BAR_HEIGHT_FALLBACK;
    bar->scale120 = (output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;

    bar->surface = wl_compositor_create_surface(wl->compositor);

    if (wl->fractional_scale_mgr) {
        bar->fractional_scale =
            wp_fractional_scale_manager_v1_get_fractional_scale(wl->fractional_scale_mgr,
                                                                bar->surface);
        wp_fractional_scale_v1_add_listener(bar->fractional_scale, &fractional_scale_listener, bar);
    }
    if (wl->viewporter)
        bar->viewport = wp_viewporter_get_viewport(wl->viewporter, bar->surface);

    bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        wl->layer_shell, bar->surface, output->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP,
        "dankc:bar");

    uint32_t edge_anchor = (cfg->bar_position == DC_BAR_POSITION_TOP)
                               ? ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP
                               : ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM;
    zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
                                     edge_anchor | ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
                                         ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, (uint32_t)bar->logical_height);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, bar->logical_height);
    zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_surface_listener, bar);

    /* Commit with no buffer to elicit the first configure. */
    wl_surface_commit(bar->surface);

    dc_info("bar created on output %s", output->model ? output->model : "?");
    return bar;
}

struct wl_surface *dc_bar_surface(dc_bar *bar)
{
    return bar->surface;
}

dc_output *dc_bar_output(dc_bar *bar)
{
    return bar->output;
}

void dc_bar_set_tray(dc_bar *bar, struct dc_tray *tray)
{
    bar->tray = tray;
}

dc_bar_region dc_bar_hittest(dc_bar *bar, double x, double y)
{
    /* The rect no longer fills the whole surface: the spacing-px gap toward
     * the outer edge (and the 4px side insets) are transparent, so a click
     * there hits nothing. */
    if (y < (double)bar->rect_y || y > (double)(bar->rect_y + bar->rect_h))
        return DC_BAR_REGION_NONE;

    double content_x0 = (double)bar_content_x0(bar);
    double content_x1 = (double)bar_content_x1(bar);

    if (x < content_x0 + 44.0)
        return DC_BAR_REGION_LAUNCHER;
    /* Notification bell + clipboard (checked before the control-center cluster). */
    if (bar->notif_cx > 0.0f && x >= bar->notif_cx - 14.0 && x <= bar->notif_cx + 14.0)
        return DC_BAR_REGION_NOTIFICATIONS;
    if (bar->clip_cx > 0.0f && x >= bar->clip_cx - 14.0 && x <= bar->clip_cx + 14.0)
        return DC_BAR_REGION_CLIPBOARD;
    if (x > content_x1 - 210.0)
        return DC_BAR_REGION_CONTROL_CENTER;
    if (x > bar->logical_width / 2.0 - 70.0 && x < bar->logical_width / 2.0 + 70.0)
        return DC_BAR_REGION_CLOCK;
    return DC_BAR_REGION_NONE;
}

void dc_bar_destroy(dc_bar *bar)
{
    if (!bar)
        return;
    if (bar->egl_ready)
        dc_egl_window_finish(&bar->egl_window, bar->egl);
    if (bar->viewport)
        wp_viewport_destroy(bar->viewport);
    if (bar->fractional_scale)
        wp_fractional_scale_v1_destroy(bar->fractional_scale);
    if (bar->layer_surface)
        zwlr_layer_surface_v1_destroy(bar->layer_surface);
    if (bar->surface)
        wl_surface_destroy(bar->surface);
    free(bar);
}
