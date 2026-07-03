#include "ui/settings.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/nvg.h"
#include "services/weather.h"
#include "theme/theme.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define DC_SET_WIDTH 760
#define DC_SET_HEIGHT 600
#define DC_SCALE_BASE 120
#define DC_SET_PAD 6.0f
#define DC_SIDEBAR_W 196.0f
#define DC_CONTENT_INSET 24.0f
#define DC_SET_THEME_COLS 5

/* Material Symbols codepoints for the sidebar + a few controls (all present in
 * the bundled full font; see docs/09 inventory / SettingsSidebar.qml icons). */
#define IC_PALETTE 0xe40a
#define IC_SCHEDULE 0xefd6
#define IC_TOOLBAR 0xe9f7
#define IC_WIDGETS 0xe1bd
#define IC_CLOUD 0xf172 /* partly_cloudy_day */
#define IC_MONITOR 0xef5b
#define IC_NOTIFICATIONS 0xe7f5
#define IC_GRID_VIEW 0xe9b0
#define IC_SECURITY 0xe32a
#define IC_INFO 0xe88e
#define IC_ADD 0xe145
#define IC_REMOVE 0xe15b
#define IC_DONE 0xe876
#define IC_LINK 0xe250

typedef enum {
    TAB_PERSONALIZATION = 0,
    TAB_TIME,
    TAB_BAR,
    TAB_WIDGETS,
    TAB_WEATHER,
    TAB_DISPLAYS,
    TAB_NOTIFICATIONS,
    TAB_LAUNCHER,
    TAB_POWER,
    TAB_ABOUT,
    TAB_COUNT,
} s_tab;

typedef struct {
    int icon;
    const char *label;
    bool implemented;
} s_tab_def;

static const s_tab_def TABS[TAB_COUNT] = {
    {IC_PALETTE, "Personalization", true},   {IC_SCHEDULE, "Time & Date", true},
    {IC_TOOLBAR, "Bar", true},               {IC_WIDGETS, "Widgets", true},
    {IC_CLOUD, "Weather", true},             {IC_MONITOR, "Displays", false},
    {IC_NOTIFICATIONS, "Notifications", true}, {IC_GRID_VIEW, "Launcher", true},
    {IC_SECURITY, "Power", false},           {IC_INFO, "About", true},
};

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

    float anim_ox, anim_oy;

    int active_tab;
    float scroll_y;
    float content_h; /* recomputed each render, drives scroll clamp */

    int focus_field; /* 0 none, 1 latitude, 2 longitude, 3 weather location, 4 wallpaper path */
    char edit_buf[256];
};

/* Bounded string copy without gcc's -Wformat-truncation firing: several text
 * fields copy between buffers of different fixed sizes (edit_buf <-> the
 * underlying config field), and snprintf(dst, sizeof(dst), "%s", src) trips
 * the warning whenever src's declared capacity exceeds dst's, even though the
 * intentional behavior here (truncate to fit) is exactly what snprintf
 * already does safely. */
static void copy_trunc(char *dst, size_t dstsz, const char *src)
{
    size_t n = strlen(src);
    if (n >= dstsz)
        n = dstsz - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}
static inline NVGcolor tc_a(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

static void recompute_physical(dc_settings *s)
{
    s->phys_width = (s->logical_width * s->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    s->phys_height = (s->logical_height * s->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

static void s_render(dc_settings *s);
static void s_teardown(dc_settings *s);

/* --- geometry helpers (must agree between render + hit-test) --- */
static float content_left(const dc_settings *s)
{
    DC_UNUSED(s);
    return DC_SET_PAD + DC_SIDEBAR_W + DC_CONTENT_INSET;
}
static float content_width(const dc_settings *s)
{
    return (float)s->logical_width - DC_SET_PAD - DC_CONTENT_INSET - content_left(s);
}
static float body_top(const dc_settings *s)
{
    DC_UNUSED(s);
    return DC_SET_PAD + 60.0f;
}
static float body_height(const dc_settings *s)
{
    return (float)s->logical_height - DC_SET_PAD - 12.0f - body_top(s);
}

/* ============================ immediate-mode UI ============================ */

typedef enum { UI_MEASURE, UI_RENDER, UI_HIT } ui_mode;

typedef struct {
    dc_settings *s;
    NVGcontext *vg;
    const dc_theme *t;
    dc_config *cfg;
    ui_mode mode;
    float w;  /* content column width (x origin is 0 in content space) */
    float y;  /* running cursor, content space */
    float cx, cy; /* click point, content space (HIT only) */
    bool hit;     /* a control consumed the click */
    bool changed; /* config mutated -> save + persist */
    bool reapply; /* theme reapply needed */
    bool weather; /* weather service re-init needed */
    bool bars;    /* bar geometry/widget reconfigure needed */
} uictx;

static bool ui_click_in(uictx *c, float x, float y, float w, float h)
{
    return c->mode == UI_HIT && !c->hit && c->cx >= x && c->cx <= x + w && c->cy >= y &&
           c->cy <= y + h;
}

static void ui_section(uictx *c, const char *label)
{
    c->y += 18.0f;
    if (c->mode == UI_RENDER) {
        nvgFontFaceId(c->vg, c->s->render->font_ui);
        nvgFontSize(c->vg, 12.0f);
        nvgTextAlign(c->vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(c->vg, tc(c->t->primary));
        nvgText(c->vg, 0, c->y + 6.0f, label, NULL);
    }
    c->y += 22.0f;
}

/* 52x30 track, thumb 24(on)/20(off) — DankToggle token spec (docs/10 §2). */
static void draw_toggle(uictx *c, float x, float cy, bool on)
{
    NVGcontext *vg = c->vg;
    const float tw = 52.0f, th = 30.0f;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, cy - th / 2.0f, tw, th, th / 2.0f);
    nvgFillColor(vg, on ? tc(c->t->primary) : tc_a(c->t->surface_variant, 90));
    nvgFill(vg);
    float r = (on ? 24.0f : 20.0f) / 2.0f;
    float thumb_x = on ? x + tw - th / 2.0f : x + th / 2.0f;
    nvgBeginPath(vg);
    nvgCircle(vg, thumb_x, cy, r);
    nvgFillColor(vg, on ? tc(c->t->surface) : tc(c->t->outline));
    nvgFill(vg);
    if (on)
        dc_render_icon(c->s->render, IC_DONE, thumb_x, cy, 15.0f, c->t->primary,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

/* Full toggle row (label + optional description + switch). Returns true if the
 * row was clicked this pass (HIT mode). */
static bool ui_toggle(uictx *c, const char *label, const char *desc, bool value)
{
    float rh = desc ? 56.0f : 46.0f;
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, c->y + (desc ? 18.0f : rh / 2.0f), label, NULL);
        if (desc) {
            nvgFontSize(vg, 12.0f);
            nvgFillColor(vg, tc(c->t->surface_variant_text));
            nvgText(vg, 0, c->y + 38.0f, desc, NULL);
        }
        draw_toggle(c, c->w - 52.0f, c->y + rh / 2.0f, value);
    }
    bool clicked = ui_click_in(c, 0, c->y, c->w, rh);
    if (clicked)
        c->hit = true;
    c->y += rh;
    return clicked;
}

/* Horizontal slider (track h12, primary fill, handle) with a label + value.
 * Mutates *value on a track click; returns true when changed. */
static bool ui_slider(uictx *c, const char *label, float *value, float lo, float hi,
                      const char *valuetext)
{
    float rh = 52.0f;
    float track_y = c->y + 36.0f;
    float frac = (*value - lo) / (hi - lo);
    if (frac < 0)
        frac = 0;
    if (frac > 1)
        frac = 1;
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, c->y + 14.0f, label, NULL);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_variant_text));
        nvgText(vg, c->w, c->y + 14.0f, valuetext, NULL);

        const float h = 12.0f;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 0, track_y - h / 2.0f, c->w, h, h / 2.0f);
        nvgFillColor(vg, tc_a(c->t->outline, 70));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 0, track_y - h / 2.0f, c->w * frac, h, h / 2.0f);
        nvgFillColor(vg, tc(c->t->primary));
        nvgFill(vg);
        nvgBeginPath(vg);
        nvgCircle(vg, c->w * frac, track_y, 10.0f);
        nvgFillColor(vg, tc(c->t->primary));
        nvgFill(vg);
    }
    bool changed = false;
    if (ui_click_in(c, -8.0f, track_y - 16.0f, c->w + 16.0f, 32.0f)) {
        float nf = c->cx / c->w;
        if (nf < 0)
            nf = 0;
        if (nf > 1)
            nf = 1;
        *value = lo + nf * (hi - lo);
        c->hit = true;
        changed = true;
    }
    c->y += rh;
    return changed;
}

/* Stepper: label left, [-] value [+] right. Mutates *value; returns changed. */
static bool ui_stepper(uictx *c, const char *label, int *value, int lo, int hi, int step)
{
    float rh = 48.0f;
    float cy = c->y + rh / 2.0f;
    const float bw = 32.0f;
    float plus_x = c->w - bw;
    float minus_x = c->w - bw - 60.0f - bw;
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, cy, label, NULL);

        for (int b = 0; b < 2; b++) {
            float bx = b == 0 ? minus_x : plus_x;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, cy - bw / 2.0f, bw, bw, 10.0f);
            nvgFillColor(vg, tc(c->t->surface_container_highest));
            nvgFill(vg);
            dc_render_icon(c->s->render, b == 0 ? IC_REMOVE : IC_ADD, bx + bw / 2.0f, cy, 18.0f,
                           c->t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%d", *value);
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, (minus_x + bw + plus_x) / 2.0f, cy, buf, NULL);
    }
    bool changed = false;
    if (ui_click_in(c, minus_x, cy - bw / 2.0f, bw, bw)) {
        *value -= step;
        if (*value < lo)
            *value = lo;
        c->hit = true;
        changed = true;
    } else if (ui_click_in(c, plus_x, cy - bw / 2.0f, bw, bw)) {
        *value += step;
        if (*value > hi)
            *value = hi;
        c->hit = true;
        changed = true;
    }
    c->y += rh;
    return changed;
}

/* Segmented control (like the battery-popout profile buttons). Returns the
 * clicked option index, or -1. */
static int ui_segmented(uictx *c, const char *label, const char *const *opts, int n, int current)
{
    float rh = 68.0f;
    float btn_y = c->y + 24.0f;
    float bh = 40.0f;
    float gap = 8.0f;
    float bw = (c->w - (n - 1) * gap) / n;
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, c->y + 8.0f, label, NULL);
        for (int i = 0; i < n; i++) {
            float bx = i * (bw + gap);
            bool sel = i == current;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, btn_y, bw, bh, 12.0f);
            nvgFillColor(vg, sel ? tc(c->t->primary) : tc(c->t->surface_container_highest));
            nvgFill(vg);
            nvgFontSize(vg, 14.0f);
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, sel ? tc(c->t->primary_text) : tc(c->t->surface_text));
            nvgText(vg, bx + bw / 2.0f, btn_y + bh / 2.0f, opts[i], NULL);
        }
    }
    int clicked = -1;
    if (c->mode == UI_HIT) {
        for (int i = 0; i < n; i++) {
            float bx = i * (bw + gap);
            if (ui_click_in(c, bx, btn_y, bw, bh)) {
                clicked = i;
                c->hit = true;
                break;
            }
        }
    }
    c->y += rh;
    return clicked;
}

/* Text field row (label + boxed value). Returns true when clicked (to focus).
 * `text` is the string to display inside the box. `focused` draws the primary
 * focus border. */
static bool ui_textfield(uictx *c, const char *label, const char *text, bool focused)
{
    float rh = 68.0f;
    float box_y = c->y + 24.0f;
    float bh = 42.0f;
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, 14.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_variant_text));
        nvgText(vg, 0, c->y + 8.0f, label, NULL);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 0, box_y, c->w, bh, 12.0f);
        nvgFillColor(vg, tc(c->t->surface_container_highest));
        nvgFill(vg);
        nvgStrokeWidth(vg, focused ? 2.0f : 1.0f);
        nvgStrokeColor(vg, focused ? tc(c->t->primary) : tc_a(c->t->outline, 90));
        nvgStroke(vg);
        nvgSave(vg);
        nvgScissor(vg, 0, box_y, c->w, bh);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 12.0f, box_y + bh / 2.0f, text && text[0] ? text : " ", NULL);
        if (focused) {
            float tw = text ? nvgTextBounds(vg, 0, 0, text, NULL, NULL) : 0.0f;
            nvgBeginPath(vg);
            nvgRect(vg, 12.0f + tw + 1.0f, box_y + 10.0f, 2.0f, bh - 20.0f);
            nvgFillColor(vg, tc(c->t->primary));
            nvgFill(vg);
        }
        nvgRestore(vg);
    }
    bool clicked = ui_click_in(c, 0, box_y, c->w, bh);
    if (clicked)
        c->hit = true;
    c->y += rh;
    return clicked;
}

static void ui_note(uictx *c, const char *text)
{
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_a(c->t->surface_variant_text, 150));
        nvgText(vg, c->w / 2.0f, 120.0f, text, NULL);
    }
    c->y += 240.0f;
}

/* ============================ bar-widget helpers ============================ */

typedef struct {
    const char *id;
    const char *label;
    int section; /* 0 left, 1 center, 2 right — default home when re-enabled */
} widget_row;

static const widget_row WIDGET_ROWS[] = {
    {"launcherButton", "Launcher", 0},   {"workspaceSwitcher", "Workspaces", 0},
    {"focusedWindow", "Focused window", 0}, {"music", "Media", 1},
    {"clock", "Clock", 1},               {"weather", "Weather", 1},
    {"systemTray", "System tray", 2},    {"clipboard", "Clipboard", 2},
    {"cpuUsage", "CPU usage", 2},        {"memUsage", "Memory usage", 2},
    {"notificationButton", "Notifications", 2}, {"battery", "Battery", 2},
    {"controlCenterButton", "Control center", 2},
};
#define WIDGET_ROWS_N ((int)(sizeof(WIDGET_ROWS) / sizeof(WIDGET_ROWS[0])))

static bool widget_in(const char arr[][DC_CONFIG_WIDGET_ID_MAX], int n, const char *id)
{
    for (int i = 0; i < n; i++)
        if (strcmp(arr[i], id) == 0)
            return true;
    return false;
}

static bool widget_enabled(const dc_config *cfg, const char *id)
{
    return widget_in(cfg->bar_left_widgets, cfg->bar_left_widgets_n, id) ||
           widget_in(cfg->bar_center_widgets, cfg->bar_center_widgets_n, id) ||
           widget_in(cfg->bar_right_widgets, cfg->bar_right_widgets_n, id);
}

static void widget_remove_from(char arr[][DC_CONFIG_WIDGET_ID_MAX], int *n, const char *id)
{
    int w = 0;
    for (int r = 0; r < *n; r++) {
        if (strcmp(arr[r], id) == 0)
            continue;
        if (w != r)
            snprintf(arr[w], DC_CONFIG_WIDGET_ID_MAX, "%s", arr[r]);
        w++;
    }
    *n = w;
}

static void widget_toggle(dc_config *cfg, const widget_row *row)
{
    if (widget_enabled(cfg, row->id)) {
        widget_remove_from(cfg->bar_left_widgets, &cfg->bar_left_widgets_n, row->id);
        widget_remove_from(cfg->bar_center_widgets, &cfg->bar_center_widgets_n, row->id);
        widget_remove_from(cfg->bar_right_widgets, &cfg->bar_right_widgets_n, row->id);
        return;
    }
    char(*arr)[DC_CONFIG_WIDGET_ID_MAX];
    int *n;
    if (row->section == 0) {
        arr = cfg->bar_left_widgets;
        n = &cfg->bar_left_widgets_n;
    } else if (row->section == 1) {
        arr = cfg->bar_center_widgets;
        n = &cfg->bar_center_widgets_n;
    } else {
        arr = cfg->bar_right_widgets;
        n = &cfg->bar_right_widgets_n;
    }
    if (*n < DC_CONFIG_WIDGETS_MAX) {
        snprintf(arr[*n], DC_CONFIG_WIDGET_ID_MAX, "%s", row->id);
        (*n)++;
    }
}

/* ============================ per-tab content ============================ */

static long rss_kb(void)
{
    FILE *f = fopen("/proc/self/statm", "r");
    if (!f)
        return -1;
    long size = 0, res = 0;
    if (fscanf(f, "%ld %ld", &size, &res) != 2) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return res * (sysconf(_SC_PAGESIZE) / 1024);
}

static void tab_personalization(uictx *c)
{
    ui_section(c, "THEME");
    /* Theme swatch grid. */
    int count = dc_theme_count();
    const float gap = 10.0f;
    const float sw = (c->w - (DC_SET_THEME_COLS - 1) * gap) / DC_SET_THEME_COLS;
    const float sh = sw * 0.55f;
    int rows = (count + DC_SET_THEME_COLS - 1) / DC_SET_THEME_COLS;
    for (int i = 0; i < count; i++) {
        int col = i % DC_SET_THEME_COLS, row = i / DC_SET_THEME_COLS;
        float x = col * (sw + gap);
        float y = c->y + row * (sh + gap);
        bool cur = strcmp(c->cfg->theme_id, dc_theme_id_at(i)) == 0;
        if (c->mode == UI_RENDER) {
            NVGcontext *vg = c->vg;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, x, y, sw, sh, 10.0f);
            nvgFillColor(vg, tc(dc_theme_primary_at(i)));
            nvgFill(vg);
            if (cur) {
                nvgBeginPath(vg);
                nvgRoundedRect(vg, x - 2.0f, y - 2.0f, sw + 4.0f, sh + 4.0f, 12.0f);
                nvgStrokeColor(vg, tc(c->t->surface_text));
                nvgStrokeWidth(vg, 2.5f);
                nvgStroke(vg);
            }
        }
        if (ui_click_in(c, x, y, sw, sh)) {
            snprintf(c->cfg->theme_id, sizeof(c->cfg->theme_id), "%s", dc_theme_id_at(i));
            c->hit = true;
            c->changed = true;
            c->reapply = true;
        }
    }
    c->y += rows * (sh + gap) + 6.0f;

    if (ui_toggle(c, "Dynamic color", "Derive the palette from the wallpaper",
                  c->cfg->dynamic_color)) {
        c->cfg->dynamic_color = !c->cfg->dynamic_color;
        c->changed = true;
        c->reapply = true;
    }
    if (c->cfg->dynamic_color) {
        bool wp_focus = c->s->focus_field == 4;
        char wpbuf[256];
        if (wp_focus)
            snprintf(wpbuf, sizeof(wpbuf), "%s", c->s->edit_buf);
        else
            copy_trunc(wpbuf, sizeof(wpbuf), c->cfg->wallpaper);
        if (ui_textfield(c, "Wallpaper image path", wpbuf, wp_focus)) {
            c->s->focus_field = 4;
            copy_trunc(c->s->edit_buf, sizeof(c->s->edit_buf), c->cfg->wallpaper);
        }
    }
    if (ui_toggle(c, "Animations", "Panel entrance/exit animations",
                  c->cfg->animations_enabled)) {
        c->cfg->animations_enabled = !c->cfg->animations_enabled;
        c->changed = true;
    }

    ui_section(c, "MOTION");
    char sv[16];
    snprintf(sv, sizeof(sv), "%.2fx", (double)c->cfg->animation_speed);
    if (ui_slider(c, "Animation speed", &c->cfg->animation_speed, 0.25f, 4.0f, sv))
        c->changed = true;
}

static void tab_time(uictx *c)
{
    ui_section(c, "CLOCK");
    if (ui_toggle(c, "24-hour format", "Use 24-hour time instead of AM/PM",
                  c->cfg->clock_24h)) {
        c->cfg->clock_24h = !c->cfg->clock_24h;
        c->changed = true;
        c->bars = true;
    }
    if (ui_toggle(c, "Show date", "Show the date beside the clock", c->cfg->show_date)) {
        c->cfg->show_date = !c->cfg->show_date;
        c->changed = true;
        c->bars = true;
    }
    if (ui_toggle(c, "Show seconds", "Display seconds in the clock", c->cfg->show_seconds)) {
        c->cfg->show_seconds = !c->cfg->show_seconds;
        c->changed = true;
        c->bars = true;
    }
}

static void tab_bar(uictx *c)
{
    ui_section(c, "POSITION");
    static const char *const pos_opts[2] = {"Top", "Bottom"};
    int cur = c->cfg->bar_position == DC_BAR_POSITION_BOTTOM ? 1 : 0;
    int clicked = ui_segmented(c, "Bar position", pos_opts, 2, cur);
    if (clicked >= 0 && clicked != cur) {
        c->cfg->bar_position = clicked == 1 ? DC_BAR_POSITION_BOTTOM : DC_BAR_POSITION_TOP;
        c->changed = true;
        c->bars = true;
    }

    ui_section(c, "SPACING");
    if (ui_stepper(c, "Screen spacing", &c->cfg->bar_spacing, 0, 32, 1)) {
        c->changed = true;
        c->bars = true;
    }
    if (ui_stepper(c, "Inner padding", &c->cfg->bar_inner_padding, 0, 32, 1)) {
        c->changed = true;
        c->bars = true;
    }
    if (ui_stepper(c, "Widget padding", &c->cfg->bar_widget_padding, 0, 32, 1)) {
        c->changed = true;
        c->bars = true;
    }

    ui_section(c, "APPEARANCE");
    char pv[16];
    snprintf(pv, sizeof(pv), "%d%%", (int)lroundf(c->cfg->bar_transparency * 100.0f));
    if (ui_slider(c, "Opacity", &c->cfg->bar_transparency, 0.0f, 1.0f, pv)) {
        c->changed = true;
        c->bars = true;
    }
    char wv[16];
    snprintf(wv, sizeof(wv), "%d%%", (int)lroundf(c->cfg->bar_widget_transparency * 100.0f));
    if (ui_slider(c, "Widget opacity", &c->cfg->bar_widget_transparency, 0.0f, 1.0f, wv)) {
        c->changed = true;
        c->bars = true;
    }
}

/* WIDGET_ROWS is grouped by section (0 left, 1 center, 2 right) already; emit
 * a section header each time it changes so the tab visually mirrors the bar's
 * L/C/R widget-host arrays (docs/08-SETTINGS-UI.md DANK BAR > Widgets). */
static const char *const WIDGET_SECTION_NAMES[3] = {"LEFT", "CENTER", "RIGHT"};

static void tab_widgets(uictx *c)
{
    int last_section = -1;
    for (int i = 0; i < WIDGET_ROWS_N; i++) {
        if (WIDGET_ROWS[i].section != last_section) {
            last_section = WIDGET_ROWS[i].section;
            ui_section(c, WIDGET_SECTION_NAMES[last_section]);
        }
        if (ui_toggle(c, WIDGET_ROWS[i].label, NULL, widget_enabled(c->cfg, WIDGET_ROWS[i].id))) {
            widget_toggle(c->cfg, &WIDGET_ROWS[i]);
            c->changed = true;
            c->bars = true;
        }
    }
}

static void tab_weather(uictx *c)
{
    ui_section(c, "WEATHER");
    if (ui_toggle(c, "Enable weather", "Show weather in the bar", c->cfg->weather_enabled)) {
        c->cfg->weather_enabled = !c->cfg->weather_enabled;
        c->changed = true;
        c->weather = true;
        c->bars = true;
    }
    if (!c->cfg->weather_enabled)
        return;
    if (ui_toggle(c, "Use Fahrenheit", "Imperial units (\xc2\xb0""F)",
                  c->cfg->weather_fahrenheit)) {
        c->cfg->weather_fahrenheit = !c->cfg->weather_fahrenheit;
        c->changed = true;
        c->weather = true;
    }

    ui_section(c, "LOCATION");
    bool name_focus = c->s->focus_field == 3;
    char namebuf[64];
    if (name_focus)
        copy_trunc(namebuf, sizeof(namebuf), c->s->edit_buf);
    else
        snprintf(namebuf, sizeof(namebuf), "%s", c->cfg->weather_location);
    if (ui_textfield(c, "Location name", namebuf, name_focus)) {
        c->s->focus_field = 3;
        snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%s", c->cfg->weather_location);
    }

    char latbuf[48], lonbuf[48];
    bool lat_focus = c->s->focus_field == 1;
    bool lon_focus = c->s->focus_field == 2;
    if (lat_focus)
        copy_trunc(latbuf, sizeof(latbuf), c->s->edit_buf);
    else
        snprintf(latbuf, sizeof(latbuf), "%.4f", c->cfg->weather_lat);
    if (lon_focus)
        copy_trunc(lonbuf, sizeof(lonbuf), c->s->edit_buf);
    else
        snprintf(lonbuf, sizeof(lonbuf), "%.4f", c->cfg->weather_lon);

    if (ui_textfield(c, "Latitude", latbuf, lat_focus)) {
        c->s->focus_field = 1;
        snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%.4f", c->cfg->weather_lat);
    }
    if (ui_textfield(c, "Longitude", lonbuf, lon_focus)) {
        c->s->focus_field = 2;
        snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%.4f", c->cfg->weather_lon);
    }
}

static void tab_notifications(uictx *c)
{
    ui_section(c, "DO NOT DISTURB");
    if (ui_toggle(c, "Do not disturb", "Suppress new toast popups (still saved to history)",
                  c->cfg->dnd_enabled)) {
        c->cfg->dnd_enabled = !c->cfg->dnd_enabled;
        c->changed = true;
    }

    ui_section(c, "POPUP TIMEOUTS (SECONDS)");
    if (ui_stepper(c, "Low urgency", &c->cfg->notif_timeout_low_sec, 1, 120, 1))
        c->changed = true;
    if (ui_stepper(c, "Normal urgency", &c->cfg->notif_timeout_normal_sec, 1, 120, 1))
        c->changed = true;
    if (ui_stepper(c, "Critical urgency (0 = never)", &c->cfg->notif_timeout_critical_sec, 0, 120,
                   5))
        c->changed = true;
}

static void tab_launcher(uictx *c)
{
    ui_section(c, "VIEW");
    static const char *const view_opts[2] = {"List", "Grid"};
    int cur = c->cfg->launcher_grid_view ? 1 : 0;
    int clicked = ui_segmented(c, "Default view mode", view_opts, 2, cur);
    if (clicked >= 0 && clicked != cur) {
        c->cfg->launcher_grid_view = clicked == 1;
        c->changed = true;
    }
}

static void tab_about(uictx *c)
{
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, 26.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, c->y + 8.0f, "DankC", NULL);
        nvgFontSize(vg, 14.0f);
        nvgFillColor(vg, tc(c->t->surface_variant_text));
        nvgText(vg, 0, c->y + 44.0f, "Version " DC_VERSION, NULL);
        nvgText(vg, 0, c->y + 66.0f,
                "A C / Wayland reimplementation of DankMaterialShell for niri.", NULL);
    }
    c->y += 100.0f;

    ui_section(c, "SYSTEM");
    long rss = rss_kb();
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        char buf[64];
        if (rss >= 0)
            snprintf(buf, sizeof(buf), "%.1f MB", rss / 1024.0);
        else
            snprintf(buf, sizeof(buf), "unavailable");
        nvgFontSize(vg, 15.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, c->y + 14.0f, "Memory usage (RSS)", NULL);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_variant_text));
        nvgText(vg, c->w, c->y + 14.0f, buf, NULL);
    }
    c->y += 44.0f;

    ui_section(c, "LINKS");
    static const char *const links[] = {"github.com/AvengeMedia/DankMaterialShell",
                                        "github.com/YaLTeR/niri"};
    for (int i = 0; i < 2; i++) {
        if (c->mode == UI_RENDER) {
            dc_render_icon(c->s->render, IC_LINK, 8.0f, c->y + 16.0f, 18.0f, c->t->primary,
                           NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgFontFaceId(c->vg, c->s->render->font_ui);
            nvgFontSize(c->vg, 14.0f);
            nvgTextAlign(c->vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
            nvgFillColor(c->vg, tc(c->t->primary));
            nvgText(c->vg, 28.0f, c->y + 16.0f, links[i], NULL);
        }
        c->y += 34.0f;
    }
}

/* focus_field values >= this edit free text (weather location, wallpaper
 * path); below it (1,2) are numeric lat/lon fields with digit-only input. */
#define DC_FOCUS_TEXT_MIN 3

static void build_tab(uictx *c)
{
    switch (c->s->active_tab) {
    case TAB_PERSONALIZATION:
        tab_personalization(c);
        break;
    case TAB_TIME:
        tab_time(c);
        break;
    case TAB_BAR:
        tab_bar(c);
        break;
    case TAB_WIDGETS:
        tab_widgets(c);
        break;
    case TAB_WEATHER:
        tab_weather(c);
        break;
    case TAB_NOTIFICATIONS:
        tab_notifications(c);
        break;
    case TAB_LAUNCHER:
        tab_launcher(c);
        break;
    case TAB_ABOUT:
        tab_about(c);
        break;
    default:
        ui_note(c, "Not implemented yet");
        break;
    }
}

/* Commit an in-progress text-field edit into the config (parse + clamp for
 * numeric fields, direct copy for free-text fields). */
static void commit_edit(dc_settings *s)
{
    if (!s->focus_field)
        return;
    dc_config *cfg = dc_config_mut();
    bool geo_changed = false;
    bool wallpaper_changed = false;
    switch (s->focus_field) {
    case 1: {
        double v = atof(s->edit_buf);
        if (v < -90.0)
            v = -90.0;
        if (v > 90.0)
            v = 90.0;
        cfg->weather_lat = v;
        geo_changed = true;
        break;
    }
    case 2: {
        double v = atof(s->edit_buf);
        if (v < -180.0)
            v = -180.0;
        if (v > 180.0)
            v = 180.0;
        cfg->weather_lon = v;
        geo_changed = true;
        break;
    }
    case 3:
        copy_trunc(cfg->weather_location, sizeof(cfg->weather_location), s->edit_buf);
        break;
    case 4:
        snprintf(cfg->wallpaper, sizeof(cfg->wallpaper), "%s", s->edit_buf);
        wallpaper_changed = true;
        break;
    default:
        break;
    }
    s->focus_field = 0;
    s->edit_buf[0] = '\0';
    dc_config_save();
    if (geo_changed && cfg->weather_enabled)
        dc_weather_init(cfg->weather_lat, cfg->weather_lon, cfg->weather_fahrenheit);
    if (wallpaper_changed && cfg->dynamic_color)
        dc_config_reapply();
    dc_config_notify_changed();
}

/* ============================ frame + render ============================ */

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

static void draw_sidebar(dc_settings *s, NVGcontext *vg, const dc_theme *t)
{
    const float x0 = DC_SET_PAD, w = DC_SIDEBAR_W;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x0, DC_SET_PAD, w, (float)s->logical_height - 2.0f * DC_SET_PAD, 16.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    nvgFontFaceId(vg, s->render->font_ui);
    nvgFontSize(vg, 18.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x0 + 16.0f, DC_SET_PAD + 28.0f, "Settings", NULL);

    const float item_h = 42.0f, top = DC_SET_PAD + 56.0f, mx = x0 + 8.0f, mw = w - 16.0f;
    for (int i = 0; i < TAB_COUNT; i++) {
        float y = top + i * item_h;
        bool active = i == s->active_tab;
        if (active) {
            nvgBeginPath(vg);
            nvgRoundedRect(vg, mx, y + 3.0f, mw, item_h - 6.0f, 12.0f);
            nvgFillColor(vg, tc_a(t->primary, 40));
            nvgFill(vg);
        }
        dc_color icol = active ? t->primary : t->surface_variant_text;
        dc_render_icon(s->render, TABS[i].icon, mx + 18.0f, y + item_h / 2.0f, 20.0f, icol,
                       NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        nvgFontFaceId(vg, s->render->font_ui);
        nvgFontSize(vg, 14.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_text));
        nvgText(vg, mx + 36.0f, y + item_h / 2.0f, TABS[i].label, NULL);
    }
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
    dc_config *cfg = dc_config_mut();
    const float w = s->logical_width, h = s->logical_height, pad = DC_SET_PAD;

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

    draw_sidebar(s, vg, t);

    /* Content header (fixed): active tab title. */
    float cl = content_left(s), cw = content_width(s), bt = body_top(s), bh = body_height(s);
    nvgFontFaceId(vg, s->render->font_ui);
    nvgFontSize(vg, 20.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, cl, pad + 30.0f, TABS[s->active_tab].label, NULL);

    /* Clamp scroll against last frame's measured content height. */
    float scroll_max = s->content_h - bh;
    if (scroll_max < 0)
        scroll_max = 0;
    if (s->scroll_y > scroll_max)
        s->scroll_y = scroll_max;
    if (s->scroll_y < 0)
        s->scroll_y = 0;

    /* Scrollable content body. */
    nvgSave(vg);
    nvgScissor(vg, cl, bt, cw + DC_CONTENT_INSET, bh);
    nvgTranslate(vg, cl, bt - s->scroll_y);
    uictx c = {.s = s, .vg = vg, .t = t, .cfg = cfg, .mode = UI_RENDER, .w = cw, .y = 0};
    build_tab(&c);
    s->content_h = c.y;
    nvgRestore(vg);

    /* Scrollbar hint. */
    if (scroll_max > 0) {
        float track_x = w - pad - 6.0f;
        float thumb_h = bh * (bh / s->content_h);
        if (thumb_h < 24.0f)
            thumb_h = 24.0f;
        float thumb_y = bt + (bh - thumb_h) * (s->scroll_y / scroll_max);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, track_x, thumb_y, 4.0f, thumb_h, 2.0f);
        nvgFillColor(vg, tc_a(t->outline, 120));
        nvgFill(vg);
    }

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
    /* TEMP(verify)-style override, like main.c's DANKC_CC_DEMO: open on a
     * specific tab for screenshot verification. */
    const char *tab_env = getenv("DANKC_SETTINGS_TAB");
    if (tab_env) {
        int ti = atoi(tab_env);
        if (ti >= 0 && ti < TAB_COUNT)
            s->active_tab = ti;
    }
    s->output = output;
    s->configured = false;
    s->egl_ready = false;
    s->scroll_y = 0;
    s->content_h = 0;
    s->focus_field = 0;
    s->edit_buf[0] = '\0';
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

    dc_popout_anchor pa = dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_CENTER, 0);
    s->anim_ox = pa.origin_x;
    s->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(s->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(s->layer_surface, DC_SET_WIDTH, DC_SET_HEIGHT);
    zwlr_layer_surface_v1_set_margin(s->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    /* On-demand keyboard so text fields (weather lat/lon) can be typed into
     * without permanently stealing focus from other apps. */
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        s->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
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
    commit_edit(s);
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

void dc_settings_handle_scroll(dc_settings *s, int steps_v)
{
    if (!s->visible || s->closing)
        return;
    s->scroll_y += steps_v * 48.0f;
    float scroll_max = s->content_h - body_height(s);
    if (scroll_max < 0)
        scroll_max = 0;
    if (s->scroll_y > scroll_max)
        s->scroll_y = scroll_max;
    if (s->scroll_y < 0)
        s->scroll_y = 0;
    s_render(s);
}

void dc_settings_handle_click(dc_settings *s, double x, double y)
{
    if (!s->visible || s->closing)
        return;

    /* Sidebar tab switch. */
    if (x >= DC_SET_PAD && x <= DC_SET_PAD + DC_SIDEBAR_W) {
        const float item_h = 42.0f, top = DC_SET_PAD + 56.0f;
        for (int i = 0; i < TAB_COUNT; i++) {
            float iy = top + i * item_h;
            if (y >= iy && y <= iy + item_h) {
                commit_edit(s);
                if (i != s->active_tab) {
                    s->active_tab = i;
                    s->scroll_y = 0;
                    s->content_h = 0;
                }
                s_render(s);
                return;
            }
        }
        return;
    }

    /* Content body: translate click into content space and run a hit pass. */
    float cl = content_left(s), cw = content_width(s), bt = body_top(s), bh = body_height(s);
    if (x < cl || x > cl + cw + DC_CONTENT_INSET || y < bt || y > bt + bh) {
        commit_edit(s);
        s_render(s);
        return;
    }

    /* Committing a stale text focus before a fresh hit (which may re-focus). */
    int prev_focus = s->focus_field;
    commit_edit(s);

    dc_config *cfg = dc_config_mut();
    uictx c = {.s = s,
               .vg = NULL,
               .t = dc_theme_current,
               .cfg = cfg,
               .mode = UI_HIT,
               .w = cw,
               .y = 0,
               .cx = (float)x - cl,
               .cy = (float)(y - bt) + s->scroll_y};
    build_tab(&c);

    if (c.changed) {
        if (c.reapply)
            dc_config_reapply();
        if (c.weather && cfg->weather_enabled)
            dc_weather_init(cfg->weather_lat, cfg->weather_lon, cfg->weather_fahrenheit);
        dc_config_save();
        if (c.bars || c.reapply)
            dc_config_notify_changed();
    }
    DC_UNUSED(prev_focus);
    s_render(s);
}

bool dc_settings_wants_keyboard(dc_settings *s)
{
    return s->visible && !s->closing && s->focus_field != 0;
}

void dc_settings_handle_key(dc_settings *s, uint32_t keysym, const char *utf8)
{
    if (!s->focus_field)
        return;
    switch (keysym) {
    case XKB_KEY_Escape:
        s->focus_field = 0;
        s->edit_buf[0] = '\0';
        s_render(s);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        commit_edit(s);
        s_render(s);
        return;
    case XKB_KEY_BackSpace: {
        size_t n = strlen(s->edit_buf);
        if (n > 0)
            s->edit_buf[n - 1] = '\0';
        s_render(s);
        return;
    }
    default:
        if (!utf8 || !utf8[0])
            return;
        if (s->focus_field < DC_FOCUS_TEXT_MIN) {
            /* Numeric fields (latitude/longitude): digits + sign/decimal only. */
            if (!(utf8[0] == '.' || utf8[0] == '-' || (utf8[0] >= '0' && utf8[0] <= '9')))
                return;
            size_t n = strlen(s->edit_buf);
            if (n + 1 < sizeof(s->edit_buf)) {
                s->edit_buf[n] = utf8[0];
                s->edit_buf[n + 1] = '\0';
                s_render(s);
            }
        } else {
            /* Free-text fields (weather location, wallpaper path): accept the
             * whole UTF-8 sequence for the key, skip control characters. */
            if ((unsigned char)utf8[0] < 0x20)
                return;
            size_t addlen = strlen(utf8);
            size_t n = strlen(s->edit_buf);
            if (n + addlen < sizeof(s->edit_buf)) {
                memcpy(s->edit_buf + n, utf8, addlen);
                s->edit_buf[n + addlen] = '\0';
                s_render(s);
            }
        }
        return;
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
