#include "ui/settings.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/apps.h"
#include "services/audio.h"
#include "services/bluez.h"
#include "services/net.h"
#include "services/power.h"
#include "services/weather.h"
#include "theme/theme.h"
#include "ui/material_bg.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* 760x600 fit the original 10 tabs; full config + system-settings coverage
 * (docs task "settings full coverage") added 4 more sidebar tabs (Dock,
 * Audio, Network, Bluetooth) which no longer fit at the old height. */
#define DC_SET_WIDTH 800
#define DC_SET_HEIGHT 720
#define DC_SCALE_BASE 120
#define DC_SET_PAD 6.0f
#define DC_SIDEBAR_W 196.0f
#define DC_CONTENT_INSET 24.0f
#define DC_SET_THEME_COLS 5

/* Sidebar tab-list geometry (docs/14-COMPLETION-PLAN.md W2 added 6 more
 * tabs, 20 total -- no longer all fit in DC_SET_HEIGHT at once, so the
 * sidebar scrolls independently of the content pane; see sidebar_scroll_y
 * below and dc_settings_handle_scroll()'s x-position routing). */
#define DC_SIDEBAR_ITEM_H 42.0f
#define DC_SIDEBAR_ITEMS_TOP (DC_SET_PAD + 56.0f)

/* Sidebar/control icon aliases -- render/icons.h owns the actual codepoints
 * (see its "Settings window" block) so scripts/subset-fonts.sh keeps the
 * bundled font subset in sync automatically. */
#define IC_PALETTE DC_ICON_PALETTE
#define IC_SCHEDULE DC_ICON_SCHEDULE
#define IC_TOOLBAR DC_ICON_TOOLBAR
#define IC_WIDGETS DC_ICON_WIDGETS
#define IC_CLOUD DC_ICON_PARTLY_CLOUDY_DAY
#define IC_MONITOR DC_ICON_MONITOR
#define IC_NOTIFICATIONS DC_ICON_NOTIFICATIONS
#define IC_GRID_VIEW DC_ICON_GRID_VIEW
#define IC_POWER DC_ICON_POWER
#define IC_INFO DC_ICON_INFO
#define IC_ADD DC_ICON_ADD
#define IC_REMOVE DC_ICON_REMOVE
#define IC_DONE DC_ICON_DONE
#define IC_LINK DC_ICON_LINK
#define IC_DOCK DC_ICON_DOCK_TO_BOTTOM
#define IC_AUDIO DC_ICON_VOLUME_UP
#define IC_NETWORK DC_ICON_WIFI
#define IC_BLUETOOTH DC_ICON_BLUETOOTH
/* docs/14-COMPLETION-PLAN.md W2 new tabs. Default Apps reuses IC_GRID_VIEW's
 * sibling APPS glyph (distinct from the launcher's grid-view icon). */
#define IC_TUNE DC_ICON_TUNE
#define IC_TEXT_FORMAT DC_ICON_TEXT_FORMAT
#define IC_COMPUTER DC_ICON_COMPUTER
#define IC_LANGUAGE DC_ICON_LANGUAGE
#define IC_APPS DC_ICON_APPS
#define IC_COLOR_LENS DC_ICON_COLOR_LENS

typedef enum {
    TAB_PERSONALIZATION = 0,
    TAB_TIME,
    TAB_TYPOGRAPHY,
    TAB_BAR,
    TAB_WIDGETS,
    TAB_WEATHER,
    TAB_DOCK,
    TAB_DISPLAYS,
    TAB_AUDIO,
    TAB_NETWORK,
    TAB_BLUETOOTH,
    TAB_NOTIFICATIONS,
    TAB_LAUNCHER,
    TAB_DEFAULT_APPS,
    TAB_LOCALE,
    TAB_SYSTEM,
    TAB_OSD,
    TAB_THEME_COLORS,
    TAB_POWER,
    TAB_ABOUT,
    TAB_COUNT,
} s_tab;

typedef struct {
    int icon;
    const char *label;
} s_tab_def;

static const s_tab_def TABS[TAB_COUNT] = {
    {IC_PALETTE, "Personalization"},     {IC_SCHEDULE, "Time & Date"},
    {IC_TEXT_FORMAT, "Typography & Motion"}, {IC_TOOLBAR, "Bar"},
    {IC_WIDGETS, "Widgets"},              {IC_CLOUD, "Weather"},
    {IC_DOCK, "Dock"},                    {IC_MONITOR, "Displays"},
    {IC_AUDIO, "Audio"},                  {IC_NETWORK, "Network"},
    {IC_BLUETOOTH, "Bluetooth"},          {IC_NOTIFICATIONS, "Notifications"},
    {IC_GRID_VIEW, "Launcher"},           {IC_APPS, "Default Apps"},
    {IC_LANGUAGE, "Locale"},              {IC_COMPUTER, "System"},
    {IC_TUNE, "OSD"},                     {IC_COLOR_LENS, "Theme & Colors"},
    {IC_POWER, "Power"},                  {IC_INFO, "About"},
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
    float sidebar_scroll_y; /* independent scroll for the (now 20-tab) sidebar list */

    int focus_field; /* 0 none, 1 latitude, 2 longitude, 3 weather location,
                      * 4 wallpaper path, 5 dock pinned-app id (add) */
    char edit_buf[256];

    bool test_clicks_done; /* DANKC_SETTINGS_CLICK consumed (see s_show) */

    /* Lazily loaded on first visit to the Default Apps tab (docs/14-
     * COMPLETION-PLAN.md W2.8) -- most sessions never open it, so this
     * avoids scanning every applications/ dir on every settings open. */
    dc_apps *apps;
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

/* Sidebar tab-list scroll geometry (mirrors content's body_top/body_height
 * pair above). Visible height is whatever's left below the "Settings"
 * header down to the card's bottom inset. */
static float sidebar_body_top(const dc_settings *s)
{
    DC_UNUSED(s);
    return DC_SIDEBAR_ITEMS_TOP;
}
static float sidebar_body_height(const dc_settings *s)
{
    return (float)s->logical_height - DC_SET_PAD - 8.0f - sidebar_body_top(s);
}
static float sidebar_items_total_h(void)
{
    return TAB_COUNT * DC_SIDEBAR_ITEM_H;
}
static float sidebar_scroll_max(const dc_settings *s)
{
    float m = sidebar_items_total_h() - sidebar_body_height(s);
    return m > 0.0f ? m : 0.0f;
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

/* Font-scale (docs/14-COMPLETION-PLAN.md W2.4, Typography & Motion tab's
 * config.h font_scale key): applied to every text size drawn by the shared
 * ui_*() widget helpers below (and tab_about()'s own text) so the slider has
 * a genuine, visible live-apply effect on Settings' own content -- see
 * config.h's font_scale comment for why this doesn't (yet) propagate to the
 * bar/other panels (hundreds of direct nvgFontSize() call sites elsewhere;
 * out of scope here per the task's own escape hatch for this item).
 * Clamped defensively; the config loader already clamps 0.8..1.5. */
static inline float ui_fs(const uictx *c, float base)
{
    float scale = c->cfg ? c->cfg->font_scale : 1.0f;
    if (scale < 0.5f)
        scale = 0.5f;
    if (scale > 2.0f)
        scale = 2.0f;
    return base * scale;
}

static void ui_section(uictx *c, const char *label)
{
    c->y += 18.0f;
    if (c->mode == UI_RENDER) {
        nvgFontFaceId(c->vg, c->s->render->font_ui);
        nvgFontSize(c->vg, ui_fs(c, 12.0f));
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
        nvgFontSize(vg, ui_fs(c, 14.0f));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, c->y + (desc ? 18.0f : rh / 2.0f), label, NULL);
        if (desc) {
            nvgFontSize(vg, ui_fs(c, 12.0f));
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
        nvgFontSize(vg, ui_fs(c, 14.0f));
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
        nvgFontSize(vg, ui_fs(c, 14.0f));
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
        nvgFontSize(vg, ui_fs(c, 14.0f));
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
        nvgFontSize(vg, ui_fs(c, 14.0f));
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
            nvgFontSize(vg, ui_fs(c, 14.0f));
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
        nvgFontSize(vg, ui_fs(c, 14.0f));
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
        nvgFontSize(vg, ui_fs(c, 14.0f));
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

/* Small left-aligned caption row (tips, "unavailable" notes). */
static void ui_hint(uictx *c, const char *text)
{
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, ui_fs(c, 12.0f));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc_a(c->t->surface_variant_text, 170));
        nvgText(vg, 0, c->y + 8.0f, text, NULL);
    }
    c->y += 24.0f;
}

/* Read-only status row: label left, value right. */
static void ui_value(uictx *c, const char *label, const char *value)
{
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, ui_fs(c, 14.0f));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, c->y + 20.0f, label, NULL);
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, tc(c->t->surface_variant_text));
        nvgText(vg, c->w, c->y + 20.0f, value, NULL);
    }
    c->y += 40.0f;
}

/* Selectable/actionable list row inside a rounded box: title (+optional
 * right-aligned status) and an optional trailing icon button. Returns 1 when
 * the row body was clicked, 2 when the trailing icon was clicked, else 0.
 * `active` paints the primary-tinted selected style. */
static int ui_list_row(uictx *c, const char *title, const char *status, int trailing_icon,
                       bool active)
{
    const float rh = 44.0f, gap = 8.0f;
    const float bw = trailing_icon ? 36.0f : 0.0f;
    float row_w = c->w - (trailing_icon ? bw + gap : 0.0f);
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, 0, c->y, row_w, rh, 12.0f);
        nvgFillColor(vg, active ? tc_a(c->t->primary, 46) : tc(c->t->surface_container_highest));
        nvgFill(vg);
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, ui_fs(c, 14.0f));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(vg, active ? tc(c->t->primary) : tc(c->t->surface_text));
        nvgSave(vg);
        nvgScissor(vg, 8.0f, c->y, row_w - (status ? 96.0f : 16.0f), rh);
        nvgText(vg, 12.0f, c->y + rh / 2.0f, title, NULL);
        nvgRestore(vg);
        if (status) {
            nvgFontSize(vg, ui_fs(c, 12.0f));
            nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
            nvgFillColor(vg, tc(c->t->surface_variant_text));
            nvgText(vg, row_w - 12.0f, c->y + rh / 2.0f, status, NULL);
        }
        if (trailing_icon) {
            float bx = c->w - bw;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, bx, c->y + (rh - 36.0f) / 2.0f, bw, 36.0f, 10.0f);
            nvgFillColor(vg, tc(c->t->surface_container_highest));
            nvgFill(vg);
            dc_render_icon(c->s->render, trailing_icon, bx + bw / 2.0f, c->y + rh / 2.0f, 18.0f,
                           c->t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }
    }
    int clicked = 0;
    if (trailing_icon && ui_click_in(c, c->w - bw, c->y, bw, rh)) {
        clicked = 2;
        c->hit = true;
    } else if (ui_click_in(c, 0, c->y, row_w, rh)) {
        clicked = 1;
        c->hit = true;
    }
    c->y += rh + 8.0f;
    return clicked;
}

/* ====================== system-settings glue (Task B) ======================
 *
 * These sections drive the *live system*, not dankc's config.json: audio via
 * wpctl (services/audio.c pattern), brightness via logind SetBrightness (the
 * unprivileged sysfs-backed setter -- busctl-style call spawned detached),
 * night mode via gammastep (same one-shot main.c's `ctl night` uses), Wi-Fi
 * radio via nmcli, Bluetooth power via bluetoothctl, power profiles via
 * services/power.c. State readers are cached a few seconds (bluez.c/net.c's
 * window) because build_tab() runs on every render AND hit pass. */

/* Run a shell command detached (children auto-reaped via main.c's SIG_IGN on
 * SIGCHLD) -- same shape as controlcenter.c's run_detached(). With
 * DANKC_SETTINGS_DRYRUN set, log the command instead of running it (same
 * offline-verification pattern as powermenu.c's DANKC_POWER_DRYRUN), so
 * destructive toggles (Wi-Fi/Bluetooth off) can be exercised safely. */
static void run_detached(const char *cmd)
{
    if (getenv("DANKC_SETTINGS_DRYRUN")) {
        dc_info("[DRYRUN] settings: would run: %s", cmd);
        return;
    }
    dc_debug("settings: run: %s", cmd);
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

#define SYS_CACHE_SECONDS 3

/* --- Default Apps (docs/14-COMPLETION-PLAN.md W2.8): browser/file-manager
 * via xdg-settings/xdg-mime, terminal via $XDG_CONFIG_HOME/xdg-terminals.list
 * (matching DMS's own DefaultAppsTab.qml -- xdg-settings has no "terminal"
 * category, xdg-terminal-exec is what actually reads that file). Reads run
 * unconditionally (side-effect-free, like wifi_radio_read() etc. above); the
 * mutating "set" commands additionally honor DANKC_XDG_DRYRUN so this tab can
 * be exercised offline without touching the user's real default apps (kept
 * as its own var per this task's explicit ask, on top of run_detached()'s
 * existing DANKC_SETTINGS_DRYRUN gate -- both are honored). */
static void run_xdg_detached(const char *cmd)
{
    if (getenv("DANKC_XDG_DRYRUN")) {
        dc_info("[XDG DRYRUN] settings: would run: %s", cmd);
        return;
    }
    run_detached(cmd);
}

/* Run `cmd`, capture its first line (trimmed) into `out`. Read-only helper
 * shared by the three xdg_default_*() readers below. */
static bool xdg_query(const char *cmd, char *out, size_t outsz)
{
    out[0] = '\0';
    FILE *pipe = popen(cmd, "r");
    if (!pipe)
        return false;
    if (fgets(out, (int)outsz, pipe)) {
        size_t n = strlen(out);
        while (n > 0 && (out[n - 1] == '\n' || out[n - 1] == '\r'))
            out[--n] = '\0';
    }
    pclose(pipe);
    return out[0] != '\0';
}

#define DC_XDG_ID_MAX 128

static bool xdg_default_browser(char *out, size_t n)
{
    static char cache[DC_XDG_ID_MAX];
    static bool cache_ok;
    static time_t cache_time;
    time_t now = time(NULL);
    if (cache_time && now - cache_time < SYS_CACHE_SECONDS) {
        snprintf(out, n, "%s", cache);
        return cache_ok;
    }
    cache_time = now;
    cache_ok = xdg_query("xdg-settings get default-web-browser 2>/dev/null", cache, sizeof(cache));
    snprintf(out, n, "%s", cache);
    return cache_ok;
}

static bool xdg_default_filemanager(char *out, size_t n)
{
    static char cache[DC_XDG_ID_MAX];
    static bool cache_ok;
    static time_t cache_time;
    time_t now = time(NULL);
    if (cache_time && now - cache_time < SYS_CACHE_SECONDS) {
        snprintf(out, n, "%s", cache);
        return cache_ok;
    }
    cache_time = now;
    cache_ok = xdg_query("xdg-mime query default inode/directory 2>/dev/null", cache,
                         sizeof(cache));
    snprintf(out, n, "%s", cache);
    return cache_ok;
}

static bool xdg_default_terminal(char *out, size_t n)
{
    static char cache[DC_XDG_ID_MAX];
    static bool cache_ok;
    static time_t cache_time;
    time_t now = time(NULL);
    if (cache_time && now - cache_time < SYS_CACHE_SECONDS) {
        snprintf(out, n, "%s", cache);
        return cache_ok;
    }
    cache_time = now;
    cache_ok = xdg_query("cat \"${XDG_CONFIG_HOME:-$HOME/.config}/xdg-terminals.list\" 2>/dev/null",
                         cache, sizeof(cache));
    snprintf(out, n, "%s", cache);
    return cache_ok;
}

static void xdg_set_browser(const char *desktop_id)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdg-settings set default-web-browser %s", desktop_id);
    run_xdg_detached(cmd);
}

static void xdg_set_filemanager(const char *desktop_id)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdg-mime default %s inode/directory", desktop_id);
    run_xdg_detached(cmd);
}

static void xdg_set_terminal(const char *desktop_id)
{
    char cmd[320];
    snprintf(cmd, sizeof(cmd),
             "mkdir -p \"${XDG_CONFIG_HOME:-$HOME/.config}\" && echo %s > "
             "\"${XDG_CONFIG_HOME:-$HOME/.config}/xdg-terminals.list\"",
             desktop_id);
    run_xdg_detached(cmd);
}

/* Heuristic category match against a desktop-entry id/exec (apps.c doesn't
 * parse Categories=; matching DMS's exact category-based filtering would
 * need extending that shared service, so this stays local to the tab --
 * see docs/14-COMPLETION-PLAN.md W2.8). Case-insensitive substring match on
 * a short curated keyword list per role. */
static bool id_matches_any(const char *id, const char *const *needles, int n)
{
    char lower[DC_APP_ID];
    size_t i = 0;
    for (; id[i] && i + 1 < sizeof(lower); i++)
        lower[i] = (char)tolower((unsigned char)id[i]);
    lower[i] = '\0';
    for (int k = 0; k < n; k++)
        if (strstr(lower, needles[k]))
            return true;
    return false;
}

/* --- backlight: read sysfs directly (controlcenter.c's read_brightness
 * pattern), set via logind's Session.SetBrightness (works unprivileged,
 * unlike writing the sysfs file; brightnessctl isn't installed here). */
typedef struct {
    char device[64]; /* e.g. "intel_backlight" */
    int cur, max;
} backlight_info;

static bool backlight_read(backlight_info *out)
{
    static backlight_info cache;
    static bool cache_ok = false;
    static time_t cache_time = 0;
    time_t now = time(NULL);
    if (cache_time && now - cache_time < SYS_CACHE_SECONDS) {
        *out = cache;
        return cache_ok;
    }
    cache_time = now;
    cache_ok = false;
    DIR *dir = opendir("/sys/class/backlight");
    if (dir) {
        struct dirent *ent;
        while ((ent = readdir(dir))) {
            if (ent->d_name[0] == '.')
                continue;
            char path[300];
            int cur = -1, max = -1;
            snprintf(path, sizeof(path), "/sys/class/backlight/%.200s/brightness", ent->d_name);
            FILE *f = fopen(path, "r");
            if (f) {
                if (fscanf(f, "%d", &cur) != 1)
                    cur = -1;
                fclose(f);
            }
            snprintf(path, sizeof(path), "/sys/class/backlight/%.200s/max_brightness",
                     ent->d_name);
            f = fopen(path, "r");
            if (f) {
                if (fscanf(f, "%d", &max) != 1)
                    max = -1;
                fclose(f);
            }
            if (cur >= 0 && max > 0) {
                copy_trunc(cache.device, sizeof(cache.device), ent->d_name);
                cache.cur = cur;
                cache.max = max;
                cache_ok = true;
                break;
            }
        }
        closedir(dir);
    }
    *out = cache;
    return cache_ok;
}

static void backlight_set(const backlight_info *bl, float frac)
{
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    unsigned raw = (unsigned)((float)bl->max * frac + 0.5f);
    char cmd[256];
    /* logind grants the session owner SetBrightness without polkit prompts;
     * "auto" resolves the caller's own session. */
    snprintf(cmd, sizeof(cmd),
             "busctl call org.freedesktop.login1 /org/freedesktop/login1/session/auto "
             "org.freedesktop.login1.Session SetBrightness ssu backlight %s %u "
             ">/dev/null 2>&1",
             bl->device, raw);
    run_detached(cmd);
}

/* --- night mode: gammastep one-shot, same command pair as main.c's `dankc
 * ctl night`. State = "a gammastep process exists". The local flip below
 * makes the toggle respond instantly instead of waiting out the cache. */
static bool night_mode_read(void)
{
    static bool cache = false;
    static time_t cache_time = 0;
    time_t now = time(NULL);
    if (cache_time && now - cache_time < SYS_CACHE_SECONDS)
        return cache;
    cache_time = now;
    cache = system("pgrep -x gammastep >/dev/null 2>&1") == 0;
    return cache;
}

static void night_mode_toggle(void)
{
    run_detached("if pgrep -x gammastep >/dev/null; then pkill -x gammastep; "
                 "else gammastep -O 4000 >/dev/null 2>&1 & fi");
}

/* --- default audio *source* (mic) mute state; audio.h's dc_audio_read()
 * only targets @DEFAULT_AUDIO_SINK@ (same split as controlcenter.c's
 * audio_source_read()). */
static bool audio_source_read(dc_audio_info *out)
{
    static dc_audio_info cache;
    static bool cache_ok = false;
    static time_t cache_time = 0;
    time_t now = time(NULL);
    if (cache_time && now - cache_time < SYS_CACHE_SECONDS) {
        *out = cache;
        return cache_ok;
    }
    cache_time = now;
    cache_ok = false;
    memset(&cache, 0, sizeof(cache));
    FILE *pipe = popen("wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null", "r");
    if (pipe) {
        char line[128];
        if (fgets(line, sizeof(line), pipe)) {
            float volume = 0.0f;
            if (sscanf(line, "Volume: %f", &volume) == 1) {
                cache.volume = (int)(volume * 100.0f + 0.5f);
                cache.available = true;
                cache_ok = true;
            }
            if (strstr(line, "MUTED"))
                cache.muted = true;
        }
        pclose(pipe);
    }
    *out = cache;
    return cache_ok;
}

/* Force the audio caches to refresh on the next read (after a mute toggle /
 * default-sink switch, so the UI reflects the change immediately instead of
 * up to SYS_CACHE_SECONDS later). dc_audio_set_volume() already invalidates
 * audio.c's own sink cache; the statics here need the same treatment. */
static time_t g_audio_dirty_until = 0;

/* --- output-device (sink) list from `wpctl status`. Parsed read-only; rows
 * switch the default via `wpctl set-default <id>`. The status output frames
 * the Audio section's sink block between "Sinks:" and the next blank-ish
 * header line; entries look like " │  *   50. Name ... [vol: 0.97]". */
#define SINKS_MAX 6

typedef struct {
    int id;
    char name[64];
    bool is_default;
} sink_entry;

static int sinks_read(sink_entry *out, int max)
{
    static sink_entry cache[SINKS_MAX];
    static int cache_n = 0;
    static time_t cache_time = 0;
    time_t now = time(NULL);
    if (cache_time && now - cache_time < SYS_CACHE_SECONDS && now >= g_audio_dirty_until) {
        int n = cache_n < max ? cache_n : max;
        memcpy(out, cache, (size_t)n * sizeof(*out));
        return n;
    }
    cache_time = now;
    g_audio_dirty_until = 0;
    cache_n = 0;
    FILE *pipe = popen("wpctl status 2>/dev/null", "r");
    if (pipe) {
        char line[256];
        bool in_audio = false, in_sinks = false;
        while (fgets(line, sizeof(line), pipe)) {
            if (strncmp(line, "Audio", 5) == 0) {
                in_audio = true;
                continue;
            }
            if (in_audio && (strncmp(line, "Video", 5) == 0 || strncmp(line, "Settings", 8) == 0))
                break;
            if (!in_audio)
                continue;
            if (strstr(line, "Sinks:")) {
                in_sinks = true;
                continue;
            }
            if (!in_sinks)
                continue;
            /* Sink entries carry an "id. name"; the section ends at the next
             * header line ("Sources:", "Filters:", ...) or a blank row. */
            if (strstr(line, "Sources:") || strstr(line, "Filters:") ||
                strstr(line, "Streams:") || strstr(line, "Devices:"))
                break;
            char *dot = strchr(line, '.');
            if (!dot)
                continue;
            /* Walk back from the dot to find the numeric id start. */
            char *p = dot;
            while (p > line && p[-1] >= '0' && p[-1] <= '9')
                p--;
            if (p == dot)
                continue; /* separator row, no id */
            int id = atoi(p);
            if (id <= 0 || cache_n >= SINKS_MAX)
                continue;
            sink_entry *e = &cache[cache_n];
            e->id = id;
            /* A '*' between the tree bars and the id marks the default. */
            e->is_default = false;
            for (char *q = line; q < p; q++)
                if (*q == '*')
                    e->is_default = true;
            /* Name: after ". ", trimmed of the trailing "[vol: ...]" tag. */
            const char *name = dot + 1;
            while (*name == ' ')
                name++;
            snprintf(e->name, sizeof(e->name), "%s", name);
            char *tag = strstr(e->name, "[vol:");
            if (tag)
                *tag = '\0';
            /* rtrim */
            size_t len = strlen(e->name);
            while (len > 0 && (e->name[len - 1] == ' ' || e->name[len - 1] == '\n'))
                e->name[--len] = '\0';
            cache_n++;
        }
        pclose(pipe);
    }
    int n = cache_n < max ? cache_n : max;
    memcpy(out, cache, (size_t)n * sizeof(*out));
    return n;
}

static void sinks_set_default(int id)
{
    char cmd[64];
    snprintf(cmd, sizeof(cmd), "wpctl set-default %d", id);
    run_detached(cmd);
    g_audio_dirty_until = time(NULL) + 1; /* re-parse shortly after the switch */
}

/* --- Wi-Fi radio state via nmcli (`nmcli radio wifi` -> "enabled"). The
 * toggle spawns `nmcli radio wifi on|off` detached; the optimistic local
 * flip keeps the switch responsive within the cache window. */
static bool wifi_radio_read(void)
{
    static bool cache = false;
    static time_t cache_time = 0;
    time_t now = time(NULL);
    if (cache_time && now - cache_time < SYS_CACHE_SECONDS)
        return cache;
    cache_time = now;
    cache = false;
    FILE *pipe = popen("nmcli radio wifi 2>/dev/null", "r");
    if (pipe) {
        char line[64];
        if (fgets(line, sizeof(line), pipe))
            cache = strncmp(line, "enabled", 7) == 0;
        pclose(pipe);
    }
    return cache;
}

/* Optimistic local state flip: the spawned command takes effect async, so
 * the next render would still show the stale cached value for up to
 * SYS_CACHE_SECONDS. flip_set() records the expected value; flip_get()
 * serves it until the cache window has passed (by which point the real
 * reader reflects the change). */
typedef struct {
    bool active;
    bool to;
    time_t at;
} opt_flip;

static bool flip_get(opt_flip *f, bool raw)
{
    if (f->active && time(NULL) - f->at < SYS_CACHE_SECONDS + 2)
        return f->to;
    f->active = false;
    return raw;
}

static void flip_set(opt_flip *f, bool to)
{
    f->active = true;
    f->to = to;
    f->at = time(NULL);
}

/* Same idea for slider values (brightness/volume): show the just-set value
 * until the reader cache catches up. */
typedef struct {
    float value;
    time_t at;
} opt_value;

static bool opt_value_get(const opt_value *v, float *out)
{
    if (v->at && time(NULL) - v->at < SYS_CACHE_SECONDS + 2) {
        *out = v->value;
        return true;
    }
    return false;
}

static void opt_value_set(opt_value *v, float value)
{
    v->value = value;
    v->at = time(NULL);
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

    /* Wallpaper drives dynamic color AND the material-blur card background,
     * so the path row is always visible (it used to hide unless dynamic
     * color was on, leaving the materialBlur source uneditable). */
    ui_section(c, "WALLPAPER");
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
    ui_hint(c, "Tip: browse and pick visually in the Dashboard's Wallpapers tab");
    if (ui_toggle(c, "Material backgrounds", "Blurred wallpaper behind panel cards",
                  c->cfg->material_blur)) {
        c->cfg->material_blur = !c->cfg->material_blur;
        c->changed = true;
        c->bars = true;
    }

    ui_section(c, "SCREEN FRAME");
    if (ui_toggle(c, "Rounded screen corners", "Draw a frame overlay rounding the display corners",
                  c->cfg->frame_enabled)) {
        c->cfg->frame_enabled = !c->cfg->frame_enabled;
        c->changed = true;
        c->bars = true; /* config_changed() also reconfigures the frames */
    }
    if (c->cfg->frame_enabled) {
        char fr[16];
        snprintf(fr, sizeof(fr), "%d px", (int)lroundf(c->cfg->frame_radius));
        if (ui_slider(c, "Corner radius", &c->cfg->frame_radius, 4.0f, 48.0f, fr)) {
            c->cfg->frame_radius = roundf(c->cfg->frame_radius);
            c->changed = true;
            c->bars = true;
        }
    }

    /* MOTION moved to its own "Typography & Motion" tab (docs/14-COMPLETION-
     * PLAN.md W2.4, matches DMS's TypographyMotionTab.qml grouping) -- see
     * tab_typography() below. */
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

/* docs/14-COMPLETION-PLAN.md W2.4 -- consolidates the MOTION section that
 * used to live at the bottom of Personalization (animations toggle + speed
 * slider, unchanged) with a new TYPOGRAPHY section, matching DMS's
 * TypographyMotionTab.qml grouping. */
static void tab_typography(uictx *c)
{
    ui_section(c, "TYPOGRAPHY");
    char fsv[16];
    snprintf(fsv, sizeof(fsv), "%.0f%%", (double)c->cfg->font_scale * 100.0);
    if (ui_slider(c, "Font scale", &c->cfg->font_scale, 0.8f, 1.5f, fsv)) {
        c->changed = true;
    }
    ui_hint(c, "Applies live to this Settings window; other panels/bar text");
    ui_hint(c, "keep their fixed size for now (full propagation deferred).");

    ui_section(c, "MOTION");
    if (ui_toggle(c, "Animations", "Panel entrance/exit animations",
                  c->cfg->animations_enabled)) {
        c->cfg->animations_enabled = !c->cfg->animations_enabled;
        c->changed = true;
    }
    char sv[16];
    snprintf(sv, sizeof(sv), "%.2fx", (double)c->cfg->animation_speed);
    if (ui_slider(c, "Animation speed", &c->cfg->animation_speed, 0.25f, 4.0f, sv))
        c->changed = true;
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

    /* docs/14-COMPLETION-PLAN.md W1.3 -- matches DMS's SoundsTab.qml "Enable
     * System Sounds" + "New Notification" rows (services/sound.c plays the
     * actual sound). */
    ui_section(c, "SOUNDS");
    if (ui_toggle(c, "Enable sounds", "Master switch for system sounds", c->cfg->sounds_enabled)) {
        c->cfg->sounds_enabled = !c->cfg->sounds_enabled;
        c->changed = true;
    }
    if (c->cfg->sounds_enabled) {
        if (ui_toggle(c, "New notification", "Play a sound when a notification arrives",
                      c->cfg->notif_sound_enabled)) {
            c->cfg->notif_sound_enabled = !c->cfg->notif_sound_enabled;
            c->changed = true;
        }
        char vv[16];
        snprintf(vv, sizeof(vv), "%d%%", (int)lroundf(c->cfg->sound_volume * 100.0f));
        if (ui_slider(c, "Volume", &c->cfg->sound_volume, 0.0f, 1.0f, vv))
            c->changed = true;
    }
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

/* Strip a trailing ".desktop" suffix for comparison against apps.c's
 * dc_app.id (which is stored without it -- see apps.c's parse comment). */
static const char *strip_desktop_suffix(const char *id, char *buf, size_t bufsz)
{
    copy_trunc(buf, bufsz, id);
    size_t n = strlen(buf);
    if (n > 8 && strcmp(buf + n - 8, ".desktop") == 0)
        buf[n - 8] = '\0';
    return buf;
}

/* One Default Apps role: current value (read-only, live xdg query) + a
 * scrollable pick-list of installed apps heuristically matching `keywords`
 * (apps.c doesn't parse Categories=, docs/14-COMPLETION-PLAN.md W2.8 --
 * matching DMS's category-based dropdown exactly would need extending that
 * shared service). Clicking a row calls `set_fn` with the ".desktop"-suffixed
 * id xdg-settings/xdg-mime expect. */
static void default_app_role(uictx *c, const char *label, bool (*get_fn)(char *, size_t),
                             void (*set_fn)(const char *), const char *const *keywords, int nkw)
{
    char cur[DC_XDG_ID_MAX];
    bool have = get_fn(cur, sizeof(cur));
    char cur_stripped[DC_XDG_ID_MAX];
    if (have)
        strip_desktop_suffix(cur, cur_stripped, sizeof(cur_stripped));
    else
        cur_stripped[0] = '\0';

    char valline[64];
    snprintf(valline, sizeof(valline), "%s", have ? cur_stripped : "(none set)");
    ui_value(c, label, valline);

    const dc_app *apps[300];
    int n = dc_apps_search(c->s->apps, "", apps, 300);
    int shown = 0;
    for (int i = 0; i < n && shown < 6; i++) {
        if (!id_matches_any(apps[i]->id, keywords, nkw))
            continue;
        bool active = have && strcmp(apps[i]->id, cur_stripped) == 0;
        if (ui_list_row(c, apps[i]->name, active ? "Default" : NULL, 0, active) == 1 &&
            !active) {
            char withdesktop[DC_APP_ID + 16];
            snprintf(withdesktop, sizeof(withdesktop), "%s.desktop", apps[i]->id);
            set_fn(withdesktop);
        }
        shown++;
    }
    if (shown == 0)
        ui_hint(c, "No installed apps matched (heuristic name match; try DANKC_XDG_DRYRUN=1)");
}

/* docs/14-COMPLETION-PLAN.md W2.8: browser/file-manager/terminal pickers.
 * Reads are always live (xdg-settings/xdg-mime/xdg-terminals.list); writes
 * are detached shell commands gated by DANKC_XDG_DRYRUN for offline
 * verification (see run_xdg_detached() above) -- this tab never has its own
 * config.json keys, same pattern as the Audio/Network/Bluetooth tabs (the
 * xdg databases and $XDG_CONFIG_HOME/xdg-terminals.list ARE the persisted
 * state; dc_config_save() has nothing to add). */
static void tab_default_apps(uictx *c)
{
    if (!c->s->apps)
        c->s->apps = dc_apps_load();

    static const char *const browser_kw[] = {"firefox",  "chromium", "chrome",     "brave",
                                             "librewolf", "vivaldi",  "opera",      "epiphany",
                                             "falkon",    "qutebrowser", "waterfox", "thorium"};
    static const char *const filemgr_kw[] = {"nautilus", "files",  "dolphin", "thunar",
                                             "nemo",      "pcmanfm", "krusader", "caja",
                                             "doublecmd", "ranger"};
    static const char *const term_kw[] = {"alacritty", "kitty",   "foot",       "konsole",
                                         "xterm",     "wezterm", "terminal",   "terminator",
                                         "tilix",     "urxvt",   "ghostty",    "contour",
                                         "xterm"};

    ui_section(c, "INTERNET");
    default_app_role(c, "Web Browser", xdg_default_browser, xdg_set_browser, browser_kw,
                     (int)(sizeof(browser_kw) / sizeof(browser_kw[0])));

    ui_section(c, "UTILITIES");
    default_app_role(c, "File Manager", xdg_default_filemanager, xdg_set_filemanager, filemgr_kw,
                     (int)(sizeof(filemgr_kw) / sizeof(filemgr_kw[0])));
    default_app_role(c, "Terminal", xdg_default_terminal, xdg_set_terminal, term_kw,
                     (int)(sizeof(term_kw) / sizeof(term_kw[0])));
    ui_hint(c, "Terminal uses xdg-terminals.list (read by xdg-terminal-exec); the");
    ui_hint(c, "others use xdg-settings/xdg-mime, same as DMS's Default Apps tab.");
}

/* docs/14-COMPLETION-PLAN.md W2 "Locale": first day of week for the
 * dashboard calendar (real config key, live-applies + persists) plus a
 * read-only locale/timezone display -- full interface translation stays
 * deferred per docs/07 G10 (this tab is UI plumbing only). */
static void tab_locale(uictx *c)
{
    ui_section(c, "CALENDAR");
    static const char *const dow_opts[2] = {"Sunday", "Monday"};
    int cur = c->cfg->first_day_of_week == 1 ? 1 : 0;
    int clicked = ui_segmented(c, "First day of week", dow_opts, 2, cur);
    if (clicked >= 0 && clicked != cur) {
        c->cfg->first_day_of_week = clicked == 1 ? 1 : 0;
        c->changed = true;
    }
    ui_hint(c, "Applies to the dashboard's calendar card");

    ui_section(c, "SYSTEM LOCALE");
    const char *lang = getenv("LANG");
    ui_value(c, "Interface language (LANG)", lang && lang[0] ? lang : "(unset)");
    const char *tz = getenv("TZ");
    if (!tz || !tz[0]) {
        static char tzbuf[64];
        ssize_t len = readlink("/etc/localtime", tzbuf, sizeof(tzbuf) - 1);
        if (len > 0) {
            tzbuf[len] = '\0';
            const char *zoneinfo = strstr(tzbuf, "zoneinfo/");
            tz = zoneinfo ? zoneinfo + 9 : tzbuf;
        }
    }
    ui_value(c, "Timezone", tz && tz[0] ? tz : "(unknown)");
    ui_hint(c, "Read-only -- change via the system locale/timezone (localectl)");
}

/* docs/14-COMPLETION-PLAN.md W2 "System": read-only host info + the handful
 * of process-lifetime toggles that are trivially available (RSS already
 * lives in About; this tab covers hostname/uptime/kernel like DMS's system
 * info rows plus the two session-wide switches that don't have a better
 * home yet). */
static void tab_system(uictx *c)
{
    ui_section(c, "HOST");
    char hostname[256] = "(unknown)";
    gethostname(hostname, sizeof(hostname) - 1);
    ui_value(c, "Hostname", hostname);

    struct utsname uts;
    if (uname(&uts) == 0) {
        char kv[192];
        snprintf(kv, sizeof(kv), "%s %s", uts.sysname, uts.release);
        ui_value(c, "Kernel", kv);
    }

    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        double secs = 0;
        if (fscanf(f, "%lf", &secs) == 1) {
            int days = (int)(secs / 86400.0);
            int hours = (int)fmod(secs / 3600.0, 24.0);
            int mins = (int)fmod(secs / 60.0, 60.0);
            char up[64];
            if (days > 0)
                snprintf(up, sizeof(up), "%dd %dh %dm", days, hours, mins);
            else
                snprintf(up, sizeof(up), "%dh %dm", hours, mins);
            ui_value(c, "Uptime", up);
        }
        fclose(f);
    }

    ui_section(c, "SESSION");
    if (ui_toggle(c, "Launch autostart apps",
                  "Run ~/.config/autostart + /etc/xdg/autostart entries at login",
                  c->cfg->autostart_enabled)) {
        c->cfg->autostart_enabled = !c->cfg->autostart_enabled;
        c->changed = true;
    }
    ui_hint(c, "Takes effect on the next login (autostart already ran this session)");
}

/* docs/14-COMPLETION-PLAN.md W2.3: OSD position + auto-hide timeout,
 * live-applying to the next volume/brightness overlay (ui/osd.c reads these
 * two config keys directly on every show/re-arm). */
static void tab_osd(uictx *c)
{
    ui_section(c, "POSITION");
    static const char *const pos_opts[4] = {"Bottom Center", "Bottom Left", "Bottom Right",
                                            "Top Center"};
    int cur = c->cfg->osd_position;
    if (cur < 0 || cur > 3)
        cur = 0;
    int clicked = ui_segmented(c, "OSD position", pos_opts, 4, cur);
    if (clicked >= 0 && clicked != cur) {
        c->cfg->osd_position = clicked;
        c->changed = true;
    }

    ui_section(c, "TIMING");
    float secs = (float)c->cfg->osd_timeout_ms / 1000.0f;
    char sv[16];
    snprintf(sv, sizeof(sv), "%.1fs", (double)secs);
    if (ui_slider(c, "Auto-hide timeout", &secs, 0.5f, 8.0f, sv)) {
        c->cfg->osd_timeout_ms = (int)lroundf(secs * 1000.0f);
        c->changed = true;
    }
    ui_hint(c, "Applies to the next volume/brightness OSD popup");
}

/* docs/14-COMPLETION-PLAN.md W2 "Theme & Colors deep tab": UI-plumbing-only
 * light/dark mode preference. dankc's theme engine (under src/theme) is
 * currently dark-only -- a separate agent owns adding real light-theme
 * variants and will consume the themeMode config key added here; this tab
 * intentionally does NOT touch any theme engine source file, matching this
 * task's explicit coordination boundary. Dynamic color itself stays on the
 * Personalization tab (unchanged) since it already existed there. */
static void tab_theme_colors(uictx *c)
{
    ui_section(c, "MODE");
    static const char *const mode_opts[2] = {"Dark", "Light"};
    int cur = strcmp(c->cfg->theme_mode, "light") == 0 ? 1 : 0;
    int clicked = ui_segmented(c, "Theme mode", mode_opts, 2, cur);
    if (clicked >= 0 && clicked != cur) {
        copy_trunc(c->cfg->theme_mode, sizeof(c->cfg->theme_mode), clicked == 1 ? "light" : "dark");
        c->changed = true;
    }
    if (cur == 1)
        ui_hint(c, "Light theme rendering isn't implemented yet -- this only");
    if (cur == 1)
        ui_hint(c, "records the preference for when it lands.");

    ui_section(c, "PALETTE");
    ui_hint(c, "Pick a color scheme and enable dynamic (wallpaper-derived)");
    ui_hint(c, "color on the Personalization tab.");
}

static void tab_dock(uictx *c)
{
    ui_section(c, "DOCK");
    if (ui_toggle(c, "Show dock", "Display a dock with pinned and running applications",
                  c->cfg->dock_enabled)) {
        c->cfg->dock_enabled = !c->cfg->dock_enabled;
        c->changed = true;
        c->bars = true; /* config_changed() maps/unmaps the dock surface */
    }
    if (c->cfg->dock_enabled) {
        if (ui_toggle(c, "Auto-hide", "Hide the dock until the pointer reaches the screen edge",
                      c->cfg->dock_auto_hide)) {
            c->cfg->dock_auto_hide = !c->cfg->dock_auto_hide;
            c->changed = true;
            c->bars = true;
        }
        if (ui_stepper(c, "Icon size", &c->cfg->dock_icon_size, 16, 96, 4)) {
            c->changed = true;
            c->bars = true;
        }
    }

    ui_section(c, "PINNED APPS");
    for (int i = 0; i < c->cfg->dock_pinned_n; i++) {
        if (ui_list_row(c, c->cfg->dock_pinned[i], NULL, IC_REMOVE, false) == 2) {
            char id[DC_CONFIG_WIDGET_ID_MAX]; /* the array compacts under us */
            snprintf(id, sizeof(id), "%s", c->cfg->dock_pinned[i]);
            widget_remove_from(c->cfg->dock_pinned, &c->cfg->dock_pinned_n, id);
            c->changed = true;
            c->bars = true;
            break; /* the list shifted -- stop iterating this pass */
        }
    }
    if (c->cfg->dock_pinned_n == 0)
        ui_hint(c, "No pinned apps yet");
    bool pin_focus = c->s->focus_field == 5;
    if (ui_textfield(c, "Pin an app (desktop-entry id, e.g. \"firefox\" -- Enter to add)",
                     pin_focus ? c->s->edit_buf : "", pin_focus)) {
        c->s->focus_field = 5;
        c->s->edit_buf[0] = '\0';
    }
    ui_hint(c, "Running apps always appear in the dock; pinning keeps them there");
}

static void tab_displays(uictx *c)
{
    ui_section(c, "BRIGHTNESS");
    backlight_info bl;
    if (backlight_read(&bl)) {
        static opt_value pending;
        float frac = (float)bl.cur / (float)bl.max;
        opt_value_get(&pending, &frac);
        char v[16];
        snprintf(v, sizeof(v), "%d%%", (int)lroundf(frac * 100.0f));
        if (ui_slider(c, "Screen brightness", &frac, 0.0f, 1.0f, v)) {
            backlight_set(&bl, frac);
            opt_value_set(&pending, frac);
        }
        char dev[128];
        snprintf(dev, sizeof(dev), "Backlight device: %.64s (set via logind)", bl.device);
        ui_hint(c, dev);
    } else {
        ui_hint(c, "No backlight device found");
    }

    ui_section(c, "NIGHT MODE");
    static opt_flip night_flip;
    bool night = flip_get(&night_flip, night_mode_read());
    if (ui_toggle(c, "Night mode", "Warm color temperature (gammastep, 4000K)", night)) {
        night_mode_toggle();
        flip_set(&night_flip, !night);
    }
}

static void tab_audio(uictx *c)
{
    ui_section(c, "OUTPUT");
    dc_audio_info out;
    bool have_out = dc_audio_read(&out);
    if (have_out) {
        static opt_value pending;
        float frac = (float)out.volume / 100.0f;
        opt_value_get(&pending, &frac);
        char v[16];
        snprintf(v, sizeof(v), "%d%%", (int)lroundf(frac * 100.0f));
        if (ui_slider(c, "Volume", &frac, 0.0f, 1.0f, v)) {
            dc_audio_set_volume((int)lroundf(frac * 100.0f));
            opt_value_set(&pending, frac);
        }
        static opt_flip mute_flip;
        bool muted = flip_get(&mute_flip, out.muted);
        if (ui_toggle(c, "Mute output", NULL, muted)) {
            run_detached("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
            flip_set(&mute_flip, !muted);
        }
    } else {
        ui_hint(c, "Audio unavailable (wpctl/WirePlumber not responding)");
    }

    ui_section(c, "OUTPUT DEVICE");
    sink_entry sinks[SINKS_MAX];
    int n = sinks_read(sinks, SINKS_MAX);
    for (int i = 0; i < n; i++) {
        if (ui_list_row(c, sinks[i].name, sinks[i].is_default ? "Default" : NULL, 0,
                        sinks[i].is_default) == 1 &&
            !sinks[i].is_default)
            sinks_set_default(sinks[i].id);
    }
    if (n == 0)
        ui_hint(c, "No output devices found");

    ui_section(c, "INPUT");
    dc_audio_info in;
    if (audio_source_read(&in)) {
        static opt_flip mic_flip;
        bool muted = flip_get(&mic_flip, in.muted);
        if (ui_toggle(c, "Mute microphone", NULL, muted)) {
            run_detached("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle");
            flip_set(&mic_flip, !muted);
        }
    } else {
        ui_hint(c, "No input device found");
    }
}

static void tab_network(uictx *c)
{
    ui_section(c, "WI-FI");
    static opt_flip wifi_flip;
    bool on = flip_get(&wifi_flip, wifi_radio_read());
    if (ui_toggle(c, "Wi-Fi enabled", "Radio on/off (nmcli)", on)) {
        run_detached(on ? "nmcli radio wifi off" : "nmcli radio wifi on");
        flip_set(&wifi_flip, !on);
    }

    ui_section(c, "STATUS");
    dc_net_info net;
    dc_net_wifi(&net);
    if (!net.has_wifi) {
        ui_value(c, "Wi-Fi adapter", "Not found");
    } else if (net.connected) {
        ui_value(c, "Connected to", net.ssid[0] ? net.ssid : "(unknown SSID)");
        if (net.signal_percent >= 0) {
            char sig[16];
            snprintf(sig, sizeof(sig), "%d%%", net.signal_percent);
            ui_value(c, "Signal", sig);
        }
    } else {
        ui_value(c, "Connection", "Disconnected");
    }
    ui_hint(c, "Scan and join networks from the Control Center's Wi-Fi section");
}

static void tab_bluetooth(uictx *c)
{
    ui_section(c, "BLUETOOTH");
    dc_bluez_info bt;
    bool have = dc_bluez_read(&bt);
    static opt_flip bt_flip;
    bool powered = flip_get(&bt_flip, have && bt.powered);
    if (ui_toggle(c, "Bluetooth enabled", "Adapter power (bluetoothctl)", powered)) {
        run_detached(powered ? "bluetoothctl power off" : "bluetoothctl power on");
        flip_set(&bt_flip, !powered);
    }

    ui_section(c, "DEVICES");
    if (!have) {
        ui_hint(c, "BlueZ unavailable");
        return;
    }
    if (!powered) {
        ui_hint(c, "Turn Bluetooth on to see paired devices");
        return;
    }
    if (bt.device_count == 0)
        ui_hint(c, "No paired devices");
    for (int i = 0; i < bt.device_count; i++) {
        const dc_bluez_device *d = &bt.devices[i];
        if (ui_list_row(c, d->name[0] ? d->name : d->mac,
                        d->connected ? "Connected" : "Paired", 0, d->connected) == 1) {
            if (d->connected)
                dc_bluez_disconnect(d->mac);
            else
                dc_bluez_connect(d->mac);
        }
    }
    ui_hint(c, "Click a device to connect or disconnect");
}

static void tab_power(uictx *c)
{
    ui_section(c, "POWER PROFILE");
    dc_power_info pw;
    if (!dc_power_read(&pw)) {
        ui_hint(c, "No power-profile backend (power-profiles-daemon or tuned) detected");
        return;
    }
    static const char *const opts[3] = {"Power Saver", "Balanced", "Performance"};
    int cur = (int)pw.active_mode; /* -1 (unknown) selects nothing */
    int clicked = ui_segmented(c, "Profile", opts, 3, cur);
    if (clicked >= 0 && clicked != cur)
        dc_power_set_mode((dc_power_mode)clicked);
    /* Raw backend profile as a caption when it isn't literally one of the 3
     * mode slugs (same rule as the battery popout's caption). */
    if (pw.active_profile[0])
        ui_value(c, "Active profile", pw.active_profile);
    ui_hint(c, "Lock, suspend and power-off live in the power menu (bar \xc2\xb7 power button)");
}

static void tab_about(uictx *c)
{
    if (c->mode == UI_RENDER) {
        NVGcontext *vg = c->vg;
        nvgFontFaceId(vg, c->s->render->font_ui);
        nvgFontSize(vg, ui_fs(c, 20.0f));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_TOP);
        nvgFillColor(vg, tc(c->t->surface_text));
        nvgText(vg, 0, c->y + 8.0f, "DankC", NULL);
        nvgFontSize(vg, ui_fs(c, 14.0f));
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
        nvgFontSize(vg, ui_fs(c, 14.0f));
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
            nvgFontSize(c->vg, ui_fs(c, 14.0f));
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
    case TAB_TYPOGRAPHY:
        tab_typography(c);
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
    case TAB_DOCK:
        tab_dock(c);
        break;
    case TAB_DISPLAYS:
        tab_displays(c);
        break;
    case TAB_AUDIO:
        tab_audio(c);
        break;
    case TAB_NETWORK:
        tab_network(c);
        break;
    case TAB_BLUETOOTH:
        tab_bluetooth(c);
        break;
    case TAB_NOTIFICATIONS:
        tab_notifications(c);
        break;
    case TAB_LAUNCHER:
        tab_launcher(c);
        break;
    case TAB_DEFAULT_APPS:
        tab_default_apps(c);
        break;
    case TAB_LOCALE:
        tab_locale(c);
        break;
    case TAB_SYSTEM:
        tab_system(c);
        break;
    case TAB_OSD:
        tab_osd(c);
        break;
    case TAB_THEME_COLORS:
        tab_theme_colors(c);
        break;
    case TAB_POWER:
        tab_power(c);
        break;
    case TAB_ABOUT:
        tab_about(c);
        break;
    default:
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
    case 5: /* dock pinned-app add (dedup'd; ids are desktop-entry basenames) */
        if (s->edit_buf[0] && cfg->dock_pinned_n < DC_CONFIG_DOCK_PINNED_MAX &&
            !widget_in(cfg->dock_pinned, cfg->dock_pinned_n, s->edit_buf)) {
            copy_trunc(cfg->dock_pinned[cfg->dock_pinned_n], DC_CONFIG_WIDGET_ID_MAX,
                       s->edit_buf);
            cfg->dock_pinned_n++;
        }
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
    nvgFontSize(vg, 20.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x0 + 16.0f, DC_SET_PAD + 28.0f, "Settings", NULL);

    const float item_h = DC_SIDEBAR_ITEM_H, top = sidebar_body_top(s), mx = x0 + 8.0f,
                mw = w - 16.0f;
    float sb_top = sidebar_body_top(s), sb_h = sidebar_body_height(s);
    nvgSave(vg);
    nvgScissor(vg, x0, sb_top, w, sb_h);
    for (int i = 0; i < TAB_COUNT; i++) {
        float y = top + i * item_h - s->sidebar_scroll_y;
        if (y + item_h < sb_top || y > sb_top + sb_h)
            continue; /* scrolled out of the visible sidebar range */
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
    nvgRestore(vg);

    /* Scrollbar hint, same visual language as the content pane's (s_render
     * below) -- only drawn when the tab list actually overflows. */
    float smax = sidebar_scroll_max(s);
    if (smax > 0.0f) {
        float track_x = x0 + w - 5.0f;
        float thumb_h = sb_h * (sb_h / sidebar_items_total_h());
        if (thumb_h < 20.0f)
            thumb_h = 20.0f;
        float thumb_y = sb_top + (sb_h - thumb_h) * (s->sidebar_scroll_y / smax);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, track_x, thumb_y, 3.0f, thumb_h, 1.5f);
        nvgFillColor(vg, tc_a(t->outline, 120));
        nvgFill(vg);
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
    /* Card: blurred+dimmed wallpaper ("material" bg) when enabled, else the
     * flat surfaceContainer fill (docs/POLISH.md P2, ui/material_bg.c). */
    dc_material_bg_fill_card(vg, s->render, pad, pad, w - 2 * pad, h - 2 * pad, 16.0f);

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

    /* TEMP(verify)-style hook, like DANKC_SETTINGS_TAB above: scripted
     * surface-local clicks ("x,y[;x,y...]") applied once after the first
     * configure, so live-apply paths can be exercised deterministically
     * offline (ydotool absolute coords are unreliable on this mixed-DPI
     * setup -- see the project's input-synthesis notes). */
    if (!s->test_clicks_done) {
        s->test_clicks_done = true;
        const char *spec = getenv("DANKC_SETTINGS_CLICK");
        while (spec && *spec) {
            double cx = 0, cy = 0;
            if (sscanf(spec, "%lf,%lf", &cx, &cy) == 2) {
                dc_info("settings: scripted click at %.0f,%.0f", cx, cy);
                dc_settings_handle_click(s, cx, cy);
            }
            spec = strchr(spec, ';');
            if (spec)
                spec++;
        }
    }
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
    /* Scroll the (now 20-tab) sidebar so the initially-selected tab is
     * visible -- matters for DANKC_SETTINGS_TAB screenshot verification of
     * tabs low in the list (logical_height is already DC_SET_HEIGHT at this
     * point; it's only overridden once the first configure event arrives). */
    {
        float item_h = DC_SIDEBAR_ITEM_H;
        float body_h = (float)s->logical_height - DC_SET_PAD - 8.0f - DC_SIDEBAR_ITEMS_TOP;
        float total_h = TAB_COUNT * item_h;
        float max_scroll = total_h - body_h;
        if (max_scroll < 0.0f)
            max_scroll = 0.0f;
        float iy = s->active_tab * item_h;
        if (iy < s->sidebar_scroll_y)
            s->sidebar_scroll_y = iy;
        else if (iy + item_h > s->sidebar_scroll_y + body_h)
            s->sidebar_scroll_y = iy + item_h - body_h;
        if (s->sidebar_scroll_y > max_scroll)
            s->sidebar_scroll_y = max_scroll;
        if (s->sidebar_scroll_y < 0.0f)
            s->sidebar_scroll_y = 0.0f;
    }
    s->focus_field = 0;
    s->edit_buf[0] = '\0';
    s->test_clicks_done = false;
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

/* `x` (surface-local, same space as dc_settings_handle_click) picks which
 * pane the wheel scrolls: over the sidebar it scrolls the (now 20-tab) tab
 * list, otherwise the active tab's content, matching the two independently-
 * scrollable regions drawn by s_render()/draw_sidebar(). */
void dc_settings_handle_scroll(dc_settings *s, double x, int steps_v)
{
    if (!s->visible || s->closing)
        return;
    if (x >= DC_SET_PAD && x <= DC_SET_PAD + DC_SIDEBAR_W) {
        s->sidebar_scroll_y += steps_v * 48.0f;
        float smax = sidebar_scroll_max(s);
        if (s->sidebar_scroll_y > smax)
            s->sidebar_scroll_y = smax;
        if (s->sidebar_scroll_y < 0)
            s->sidebar_scroll_y = 0;
        s_render(s);
        return;
    }
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
        const float item_h = DC_SIDEBAR_ITEM_H, top = DC_SIDEBAR_ITEMS_TOP;
        float sb_top = sidebar_body_top(s), sb_h = sidebar_body_height(s);
        if (y < sb_top || y > sb_top + sb_h)
            return; /* click on the header/outside the scrolled tab list */
        for (int i = 0; i < TAB_COUNT; i++) {
            float iy = top + i * item_h - s->sidebar_scroll_y;
            if (iy + item_h < sb_top || iy > sb_top + sb_h)
                continue; /* scrolled out of view */
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
    if (s->apps)
        dc_apps_destroy(s->apps);
    free(s);
}
