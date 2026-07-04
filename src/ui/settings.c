#include "ui/settings.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "services/apps.h"
#include "services/audio.h"
#include "services/battery.h"
#include "services/bluez.h"
#include "services/display.h"
#include "services/firewall.h"
#include "services/keybinds.h"
#include "services/logind.h"
#include "services/net.h"
#include "services/nightlight.h"
#include "services/niri_input.h"
#include "services/notifications.h"
#include "services/power.h"
#include "services/printers.h"
#include "services/systheme.h"
#include "services/timedate.h"
#include "services/updates.h"
#include "services/weather.h"
#include "theme/theme.h"
#include "ui/connected.h"
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
#include <strings.h>
#include <pwd.h>
#include <sys/stat.h>
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

/* Card-fill padding (docs/27-CONNECTED-FRAME-PLAN.md): floating chrome
 * reserves a flat 6px of shadow room on all four sides; connected chrome
 * widens the lateral (side) pad to 12 for the connector fillets and drops
 * the bar-facing (near) pad to 0, leaving the far pad at 6 -- see
 * dc_popout_chrome_pads(). Which physical edge is "near" vs "far" swaps
 * with bar_position, so top/bottom below are resolved from that (same
 * convention as controlcenter.c's cc_get_layout() / processes.c's
 * ps_get_layout()). These recompute from dc_config_current on every call
 * (cheap: two ints + a branch) rather than caching, since settings.c's
 * geometry helpers (content_left/body_top/etc.) are called from many sites
 * across this file and none of them carry a layout struct today. */
static float sp_pad_side(void)
{
    int pad_side;
    dc_popout_chrome_pads(dc_config_current, NULL, &pad_side, NULL);
    return (float)pad_side;
}
static bool sp_bottom_bar(void)
{
    return dc_config_current->bar_position == DC_BAR_POSITION_BOTTOM;
}
static float sp_pad_top(void)
{
    int pad_near, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, NULL, &pad_far);
    return sp_bottom_bar() ? (float)pad_far : (float)pad_near;
}
static float sp_pad_bottom(void)
{
    int pad_near, pad_far;
    dc_popout_chrome_pads(dc_config_current, &pad_near, NULL, &pad_far);
    return sp_bottom_bar() ? (float)pad_near : (float)pad_far;
}

/* Logical surface width. DC_SET_WIDTH already bakes in the floating
 * chrome's flat 6px pad on every side; connected_frame widens the lateral
 * (side) pad to 12 for the connector fillets, so the surface needs
 * 2*(pad_side-6) more logical px to keep the card CONTENT rect -- inset by
 * pad_side, see content_left()/content_width() -- exactly where it sits
 * when floating (mirrors controlcenter.c's cc_surface_width()).
 * connected_frame off: pad_side==6, so this is just DC_SET_WIDTH,
 * unchanged. Height is untouched: it's a fixed constant (DC_SET_HEIGHT)
 * rather than content-driven like controlcenter's, and the near/far pad
 * split nets to a *smaller* total (6 vs 12) when connected, so no widening
 * is needed there. */
static int sp_surface_width(void)
{
    int pad_side = 6;
    dc_popout_chrome_pads(dc_config_current, NULL, &pad_side, NULL);
    return DC_SET_WIDTH + 2 * (pad_side - 6);
}

/* Sidebar tab-list geometry (docs/14-COMPLETION-PLAN.md W2 added 6 more
 * tabs, 20 total -- no longer all fit in DC_SET_HEIGHT at once, so the
 * sidebar scrolls independently of the content pane; see sidebar_scroll_y
 * below and dc_settings_handle_scroll()'s x-position routing). */
#define DC_SIDEBAR_ITEM_H 42.0f
/* Was a flat DC_SET_PAD+56 macro; now sidebar_body_top()'s near-edge pad
 * varies with bar_position/connected_frame, so every former
 * DC_SIDEBAR_ITEMS_TOP reference below calls that function instead. */

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
/* docs/14-COMPLETION-PLAN.md W3.2/W3.4: Lock Screen reuses the existing LOCK
 * glyph (already used by ui/lock.c's password pill); Window Rules gets its
 * own new glyph (render/icons.h). */
#define IC_LOCK_SCREEN DC_ICON_LOCK
#define IC_WINDOW_RULES DC_ICON_SELECT_WINDOW
/* docs/14-COMPLETION-PLAN.md W5 stretch tabs. Users reuses the existing
 * PERSON glyph (already used elsewhere), so no new render/icons.h entry was
 * needed for it. */
#define IC_MUX DC_ICON_TERMINAL
#define IC_SYSTEM_UPDATER DC_ICON_UPDATE
#define IC_PRINTER DC_ICON_PRINT
#define IC_USERS DC_ICON_PERSON
/* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.3: Displays tab's resolution
 * picker expand/collapse chevron -- both glyphs already exist in
 * render/icons.h (used nowhere else yet), no font-subset update needed. */
#define IC_EXPAND_MORE DC_ICON_EXPAND_MORE
#define IC_EXPAND_LESS DC_ICON_EXPAND_LESS
/* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.4: Night Light gets its own tab
 * (replaces the old duplicated pgrep/pkill/gammastep toggle that used to live
 * in tab_displays). Glyph already used by controlcenter.c's "Night Mode"
 * tile -- already in the bundled font subset, no scripts/subset-fonts.sh
 * update needed. */
#define IC_NIGHTLIGHT DC_ICON_NIGHTLIGHT
/* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.5/7/8/9: Firewall (own glyph),
 * Mouse/Touchpad/Keyboard (reuses the existing MOUSE glyph, already in the
 * font subset via the Bluetooth device-type icons). Date & Time and Power
 * idle/lid extend the existing TAB_TIME/TAB_POWER tabs, no new icon needed. */
#define IC_SHIELD DC_ICON_SHIELD
#define IC_MOUSE DC_ICON_MOUSE
/* docs/23-KEYBIND-EDITING-PLAN.md sec.1/KB-T3: Keybinds tab. Reuses
 * DC_ICON_KEYBOARD, already in the font subset via controlcenter.c's device
 * icon for keyboard input sources. */
#define IC_KEYBOARD DC_ICON_KEYBOARD

typedef enum {
    TAB_PERSONALIZATION = 0,
    TAB_TIME,
    TAB_TYPOGRAPHY,
    TAB_BAR,
    TAB_WIDGETS,
    TAB_WEATHER,
    TAB_DOCK,
    TAB_DISPLAYS,
    TAB_NIGHTLIGHT,
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
    TAB_LOCKSCREEN,
    TAB_WINDOW_RULES,
    TAB_KEYBINDS,
    TAB_MUX,
    TAB_SYSTEM_UPDATER,
    TAB_PRINTER,
    /* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.5/7/9: Firewall (ufw/
     * firewalld) and Mouse/Touchpad/Keyboard (niri input fragment). Date &
     * Time and Power idle/lid extend TAB_TIME/TAB_POWER above instead of
     * adding new tabs (avoids sidebar bloat -- see those tabs' bodies). */
    TAB_FIREWALL,
    TAB_INPUT,
    TAB_USERS,
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
    {IC_NIGHTLIGHT, "Night Light"},
    {IC_AUDIO, "Audio"},                  {IC_NETWORK, "Network"},
    {IC_BLUETOOTH, "Bluetooth"},          {IC_NOTIFICATIONS, "Notifications"},
    {IC_GRID_VIEW, "Launcher"},           {IC_APPS, "Default Apps"},
    {IC_LANGUAGE, "Locale"},              {IC_COMPUTER, "System"},
    {IC_TUNE, "OSD"},                     {IC_COLOR_LENS, "Theme & Colors"},
    {IC_POWER, "Power"},                  {IC_LOCK_SCREEN, "Lock Screen"},
    {IC_WINDOW_RULES, "Window Rules"},    {IC_KEYBOARD, "Keybinds"},
    {IC_MUX, "Multiplexer"},
    {IC_SYSTEM_UPDATER, "System Updater"}, {IC_PRINTER, "Printer"},
    {IC_SHIELD, "Firewall"},               {IC_MOUSE, "Mouse & Keyboard"},
    {IC_USERS, "Users"},                  {IC_INFO, "About"},
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
                      * 4 wallpaper path, 5 dock pinned-app id (add), 15 audio
                      * device rename (see audio_rename_target below) */
    char edit_buf[256];

    bool test_clicks_done; /* DANKC_SETTINGS_CLICK consumed (see s_show) */

    /* Lazily loaded on first visit to the Default Apps tab (docs/14-
     * COMPLETION-PLAN.md W2.8) -- most sessions never open it, so this
     * avoids scanning every applications/ dir on every settings open. */
    dc_apps *apps;

    /* niri Window Rules tab draft (docs/14-COMPLETION-PLAN.md W3.4): the
     * in-progress "add a rule" form. Not part of dc_config/config.json --
     * see wr_add_rule() below, which writes ~/.config/niri/dankc-rules.kdl
     * instead. app-id text editing goes through the shared focus_field/
     * edit_buf mechanism (focus_field == 6), committed here on Enter/blur. */
    char wr_new_app_id[160];
    bool wr_new_floating;
    bool wr_new_maximized;
    bool wr_new_opacity_enabled;
    float wr_new_opacity;

    /* Keybinds tab (docs/23-KEYBIND-EDITING-PLAN.md sec.1/3, KB-T3): the
     * in-progress "add a bind" form + chord-capture state. Not dc_config/
     * config.json state -- dc_keybinds_load()/persist() (services/keybinds.h,
     * KB-T1) own the actual ~/.config/niri/dankc-binds.kdl fragment, this is
     * purely the draft the UI is building before it calls persist(). */
    bool kb_capture;              /* true while a zwp_keyboard_shortcuts_inhibitor
                                    * is active and dc_settings_handle_key() is
                                    * consuming every key as chord capture */
    char kb_new_chord[64];        /* DC_KEYBIND_CHORD_MAX, captured chord, "" if
                                    * none recorded yet this add-form session */
    int kb_mode;                  /* 0 niri action, 1 dankc action, 2 custom cmd */
    int kb_niri_idx;               /* selected index into dc_keybinds_niri_actions(),
                                    * -1 none chosen */
    int kb_dankc_idx;             /* selected index into dc_keybinds_dankc_actions() */
    char kb_custom_cmd[160];      /* mode 2: free shell command, spawned via sh -c */
    char kb_title[96];            /* DC_KEYBIND_TITLE_MAX: optional hotkey-overlay-title */

    /* Displays tab (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.3): which
     * monitor card's detail section is expanded, and whether that monitor's
     * resolution picker is expanded. Not config.json state -- dc_display_
     * list() is the live source of truth, this is purely which UI
     * disclosure is open right now. */
    int disp_selected;
    bool disp_res_open;

    /* Network tab hotspot form (docs/19 sec.2, "Hotspot" section extension):
     * draft SSID/password, not persisted anywhere until "Start Hotspot" is
     * clicked -- dc_net_hotspot_start() is the actual owner of the resulting
     * NetworkManager connection, this is just the in-progress form state. */
    char net_hotspot_ssid[64];
    char net_hotspot_password[64];

    /* Time & Date tab (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.8): timezone
     * picker disclosure + filter draft. Not config.json state --
     * dc_timedate_status()/dc_timedate_set_timezone() are the live source of
     * truth, same shape as the Displays tab's disp_selected/disp_res_open. */
    bool tz_picker_open;
    char tz_filter[64];

    /* Audio tab (docs/25-AUDIO-PERDEVICE-PLAN.md T4): node.name of whichever
     * sink/source's "Rename" textfield is mid-edit (focus_field == 15),
     * committed via dc_config_audio_set_alias() in commit_edit(). Shared by
     * both the OUTPUT and INPUT cards since focus_field is single-valued --
     * only one textfield can ever be focused at a time. Not config.json
     * state itself, just the in-progress edit target (same shape as
     * net_hotspot_ssid's draft-buffer above). */
    char audio_rename_target[DC_CONFIG_AUDIO_NAME_MAX];
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
    return sp_pad_side() + DC_SIDEBAR_W + DC_CONTENT_INSET;
}
static float content_width(const dc_settings *s)
{
    return (float)s->logical_width - sp_pad_side() - DC_CONTENT_INSET - content_left(s);
}
static float body_top(const dc_settings *s)
{
    DC_UNUSED(s);
    return sp_pad_top() + 60.0f;
}
static float body_height(const dc_settings *s)
{
    return (float)s->logical_height - sp_pad_bottom() - 12.0f - body_top(s);
}

/* Sidebar tab-list scroll geometry (mirrors content's body_top/body_height
 * pair above). Visible height is whatever's left below the "Settings"
 * header down to the card's bottom inset. */
static float sidebar_body_top(const dc_settings *s)
{
    DC_UNUSED(s);
    return sp_pad_top() + 56.0f;
}
static float sidebar_body_height(const dc_settings *s)
{
    return (float)s->logical_height - sp_pad_bottom() - 8.0f - sidebar_body_top(s);
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

/* Scroll the sidebar tab list just enough to bring s->active_tab into view
 * (used both when the window first opens on a tab -- e.g. DANKC_SETTINGS_TAB,
 * dc_settings_toggle_tab() -- and when switching tabs while already open).
 * Extracted so both call sites share the same clamp logic. */
static void sidebar_reveal_active_tab(dc_settings *s)
{
    float item_h = DC_SIDEBAR_ITEM_H;
    float body_h = sidebar_body_height(s);
    float max_scroll = sidebar_scroll_max(s);
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

static void xdg_set_terminal(const char *desktop_id)
{
    char cmd[320];
    snprintf(cmd, sizeof(cmd),
             "mkdir -p \"${XDG_CONFIG_HOME:-$HOME/.config}\" && echo %s > "
             "\"${XDG_CONFIG_HOME:-$HOME/.config}/xdg-terminals.list\"",
             desktop_id);
    run_xdg_detached(cmd);
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

    /* docs/27-CONNECTED-FRAME-PLAN.md T1: config-only for now -- no panel
     * reads connected_frame yet, so flipping this has no visible effect
     * until T2+ wires up the chrome rendering. Independent of
     * frame_enabled above (screen-corner overlay). */
    if (ui_toggle(c, "Connected panels", "Stitch popouts and the dock into the bar",
                  c->cfg->connected_frame)) {
        c->cfg->connected_frame = !c->cfg->connected_frame;
        c->changed = true;
        c->bars = true;
    }

    /* MOTION moved to its own "Typography & Motion" tab (docs/14-COMPLETION-
     * PLAN.md W2.4, matches DMS's TypographyMotionTab.qml grouping) -- see
     * tab_typography() below. */
}

/* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.8: timezone list is fetched once
 * per process (~600 entries, doesn't change at runtime) and cached here --
 * build_tab() runs on every render AND every hit-test pass, so re-popen()ing
 * `timedatectl list-timezones` per call would be wasteful. */
#define DC_TZ_CACHE_MAX 700
static char g_tz_cache[DC_TZ_CACHE_MAX][DC_TIMEDATE_TZ_MAX];
static int g_tz_cache_n = -1;

static void tz_ensure_cache(void)
{
    if (g_tz_cache_n < 0)
        g_tz_cache_n = dc_timedate_list_timezones(g_tz_cache, DC_TZ_CACHE_MAX);
}

static bool str_ci_contains(const char *hay, const char *needle)
{
    if (!needle || !needle[0])
        return true;
    size_t hn = strlen(hay), nn = strlen(needle);
    if (nn > hn)
        return false;
    for (size_t i = 0; i + nn <= hn; i++) {
        size_t j = 0;
        for (; j < nn; j++)
            if (tolower((unsigned char)hay[i + j]) != tolower((unsigned char)needle[j]))
                break;
        if (j == nn)
            return true;
    }
    return false;
}

/* Timezone picker's max rendered matches -- bounds draw/hit-test cost when
 * the filter is broad (e.g. empty, which would otherwise list all ~600). */
#define DC_TZ_MAX_SHOWN 30

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

    ui_section(c, "SYSTEM DATE & TIME");
    dc_timedate_info td;
    if (!dc_timedate_status(&td)) {
        ui_hint(c, "timedatectl not found -- system clock/timezone management");
        ui_hint(c, "is unavailable (needs systemd).");
        return;
    }

    if (td.now_local[0])
        ui_value(c, "Local time", td.now_local);
    ui_value(c, "Time zone", td.timezone[0] ? td.timezone : "(unknown)");
    ui_value(c, "Clock synchronized", td.ntp_synchronized ? "Yes" : "No");

    if (td.can_ntp) {
        static opt_flip ntp_flip;
        bool ntp_on = flip_get(&ntp_flip, td.ntp_enabled);
        if (ui_toggle(c, "Automatic time sync (NTP)", "Keep the clock synced over the network",
                      ntp_on)) {
            dc_timedate_set_ntp(!ntp_on);
            flip_set(&ntp_flip, !ntp_on);
        }
    } else {
        ui_hint(c, "No NTP service available to timedatectl on this system.");
    }

    if (ui_list_row(c, c->s->tz_picker_open ? "Cancel changing time zone" : "Change time zone",
                    NULL, 0, c->s->tz_picker_open) == 1)
        c->s->tz_picker_open = !c->s->tz_picker_open;

    if (c->s->tz_picker_open) {
        tz_ensure_cache();

        bool filt_focus = c->s->focus_field == 12;
        char filtbuf[64];
        if (filt_focus)
            copy_trunc(filtbuf, sizeof(filtbuf), c->s->edit_buf);
        else
            snprintf(filtbuf, sizeof(filtbuf), "%s", c->s->tz_filter);
        if (ui_textfield(c, "Filter (e.g. \"Asia\" or \"Karachi\")", filtbuf, filt_focus)) {
            c->s->focus_field = 12;
            snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%s", c->s->tz_filter);
        }

        if (g_tz_cache_n <= 0) {
            ui_hint(c, "`timedatectl list-timezones` returned nothing.");
        } else {
            int shown = 0;
            for (int i = 0; i < g_tz_cache_n && shown < DC_TZ_MAX_SHOWN; i++) {
                if (!str_ci_contains(g_tz_cache[i], c->s->tz_filter))
                    continue;
                bool active = strcmp(g_tz_cache[i], td.timezone) == 0;
                if (ui_list_row(c, g_tz_cache[i], NULL, 0, active) == 1 && !active) {
                    dc_timedate_set_timezone(g_tz_cache[i]);
                    c->s->tz_picker_open = false;
                }
                shown++;
            }
            if (shown == DC_TZ_MAX_SHOWN)
                ui_hint(c, "More matches exist -- narrow the filter to see them.");
            else if (shown == 0)
                ui_hint(c, "No time zones match that filter.");
        }
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

/* Format dc_notif_dnd_remaining_sec()'s result as a one-line status for the
 * DND section hint (docs/26-DND-SCHEDULING-PLAN.md UI). Mirrors notifcenter.
 * c's countdown label, just spelled out in full since Settings has room. */
static void dnd_countdown_label(char *out, size_t outsz)
{
    int64_t rem = dc_notif_dnd_remaining_sec();
    if (rem < 0) {
        snprintf(out, outsz, "Do Not Disturb is off");
    } else if (rem == 0) {
        snprintf(out, outsz, "Do Not Disturb is on (no end time)");
    } else {
        int h = (int)(rem / 3600);
        int m = (int)((rem % 3600) / 60);
        int s = (int)(rem % 60);
        if (h > 0)
            snprintf(out, outsz, "Resumes in %dh %dm", h, m);
        else if (m > 0)
            snprintf(out, outsz, "Resumes in %dm %ds", m, s);
        else
            snprintf(out, outsz, "Resumes in %ds", s);
    }
}

/* Per-app rule action/urgency segmented option labels (docs/26-DND-
 * SCHEDULING-PLAN.md rule-engine section) -- indices match core/config.h's
 * dc_notif_rule.action (0-3) / .urgency (-1..2, offset by +1 for the array). */
static const char *const RULE_ACTION_OPTS[4] = {"Mute", "Ignore", "Popup only", "No history"};
static const char *const RULE_URGENCY_OPTS[4] = {"Keep", "Low", "Normal", "Critical"};

/* Case-insensitive dedup check against existing rules' match strings, same
 * comparison notifications.c's method_notify() rule evaluator uses. */
static bool notif_rule_match_exists(const dc_notif_rule *arr, int n, const char *match)
{
    for (int i = 0; i < n; i++)
        if (strcasecmp(arr[i].match, match) == 0)
            return true;
    return false;
}

/* Compact-remove rule `idx`, shifting later entries down (same pattern as
 * widget_remove_from() above, just for a struct array instead of a char[][]
 * one). */
static void notif_rule_remove_at(dc_config *cfg, int idx)
{
    for (int i = idx; i + 1 < cfg->notif_rules_n; i++)
        cfg->notif_rules[i] = cfg->notif_rules[i + 1];
    cfg->notif_rules_n--;
}

static void tab_notifications(uictx *c)
{
    ui_section(c, "DO NOT DISTURB");
    if (ui_toggle(c, "Do not disturb", "Suppress new toast popups (still saved to history)",
                  c->cfg->dnd_enabled)) {
        c->cfg->dnd_enabled = !c->cfg->dnd_enabled;
        c->changed = true;
    }

    /* Quick presets (docs/26-DND-SCHEDULING-PLAN.md UI) -- mirrors
     * notifcenter.c's DND chip row (Off|15m|1h|Until <hour>|Forever). These
     * call straight into the dc_notif_dnd_* API (notifications.h), which
     * mutates + saves core/config.h's dnd_enabled/dnd_until_epoch itself, so
     * no c->changed is strictly required for persistence -- it's still set
     * so the panel redraws immediately with the new toggle/countdown state. */
    char hour_label[16];
    snprintf(hour_label, sizeof(hour_label), "Until %d:00", c->cfg->dnd_until_hour);
    const char *preset_opts[5] = {"Off", "15m", "1h", hour_label, "Forever"};
    int64_t remaining = dc_notif_dnd_remaining_sec();
    int dnd_cur = remaining < 0 ? 0 : (remaining == 0 ? 4 : -1);
    int dnd_clicked = ui_segmented(c, "Quick presets", preset_opts, 5, dnd_cur);
    if (dnd_clicked >= 0) {
        switch (dnd_clicked) {
        case 0:
            dc_notif_dnd_stop(NULL);
            break;
        case 1:
            dc_notif_dnd_start(NULL, 15 * 60);
            break;
        case 2:
            dc_notif_dnd_start(NULL, 60 * 60);
            break;
        case 3:
            dc_notif_dnd_start_until_hour(NULL, c->cfg->dnd_until_hour);
            break;
        case 4:
            dc_notif_dnd_start(NULL, 0);
            break;
        }
        c->changed = true;
    }
    if (ui_stepper(c, "\"Until\" resume hour", &c->cfg->dnd_until_hour, 0, 23, 1))
        c->changed = true;
    char dnd_hint[64];
    dnd_countdown_label(dnd_hint, sizeof(dnd_hint));
    ui_hint(c, dnd_hint);

    ui_section(c, "PRIVACY");
    if (ui_toggle(c, "Hide notification content in popups",
                  "Toast popups show \"New notification\" instead of the summary/body "
                  "(the notification center still shows everything)",
                  c->cfg->notif_privacy_mode)) {
        c->cfg->notif_privacy_mode = !c->cfg->notif_privacy_mode;
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

    /* Per-app rules (docs/26-DND-SCHEDULING-PLAN.md rule-engine section):
     * first-match-wins list evaluated in notifications.c's method_notify()
     * against app_name/desktop-entry, case-insensitively. Each row is its own
     * match label + action/urgency segmented pair + a remove-X list row, same
     * "one row body, shifted removal" shape as tab_dock()'s pinned-apps list
     * above. */
    ui_section(c, "PER-APP RULES");
    for (int i = 0; i < c->cfg->notif_rules_n; i++) {
        dc_notif_rule *r = &c->cfg->notif_rules[i];
        if (ui_list_row(c, r->match, RULE_ACTION_OPTS[r->action], IC_REMOVE, false) == 2) {
            notif_rule_remove_at(c->cfg, i);
            c->changed = true;
            break; /* the array compacted under us -- stop iterating this pass */
        }
        int action_clicked = ui_segmented(c, "Action", RULE_ACTION_OPTS, 4, r->action);
        if (action_clicked >= 0 && action_clicked != r->action) {
            r->action = action_clicked;
            c->changed = true;
        }
        int urgency_clicked = ui_segmented(c, "Urgency override", RULE_URGENCY_OPTS, 4,
                                           r->urgency + 1);
        if (urgency_clicked >= 0 && urgency_clicked != r->urgency + 1) {
            r->urgency = urgency_clicked - 1;
            c->changed = true;
        }
    }
    if (c->cfg->notif_rules_n == 0)
        ui_hint(c, "No per-app rules yet");
    bool rule_focus = c->s->focus_field == 16;
    if (ui_textfield(c,
                     "Add a rule (app name or desktop-entry id, e.g. \"discord\" -- Enter to add)",
                     rule_focus ? c->s->edit_buf : "", rule_focus)) {
        c->s->focus_field = 16;
        c->s->edit_buf[0] = '\0';
    }
    ui_hint(c, "New rules default to Mute / Keep urgency -- adjust above after adding");
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

/* One Default Apps category (docs/17-DEFAULT-APPS-PLAN.md sec 5.1). mimes[0]
 * is the primary MIME type (used for `xdg-mime query default`/enumeration
 * reads and as the first arg to the batched write); mimes[1..nmimes-1] are
 * "also set" MIME types folded into that same write. `category`, if non-NULL,
 * switches BOTH candidate enumeration (dc_apps_find_by_category() instead of
 * dc_apps_find_by_mime()) to a Categories= token -- used for the three roles
 * where plain MimeType= matching is the wrong tool (Web Browser/File Manager/
 * Terminal, see docs/17 sec 1.6/1.7); reads/writes for these still go through
 * their own special mechanism per is_browser/is_terminal below. */
typedef struct {
    const char *label;   /* UI row label, e.g. "Web Browser" */
    const char *section; /* ui_section() header this row falls under */
    const char *const *mimes;
    int nmimes;
    const char *category; /* NULL => enumerate by mimes[0] instead */
    bool is_browser;       /* true only for the one row using xdg-settings */
    bool is_terminal;      /* true only for the one row using xdg-terminals.list */
} dc_default_app_category;

static const char *const cat_browser_mimes[] = {
    "x-scheme-handler/http",
    "x-scheme-handler/https",
    "text/html",
    "application/xhtml+xml",
};
static const char *const cat_mailto_mimes[] = {"x-scheme-handler/mailto"};
static const char *const cat_calendar_mimes[] = {"text/calendar", "x-scheme-handler/calendar"};
static const char *const cat_rss_mimes[] = {"application/rss+xml", "application/atom+xml"};
static const char *const cat_geo_mimes[] = {"x-scheme-handler/geo"};
static const char *const cat_torrent_mimes[] = {"application/x-bittorrent",
                                                "x-scheme-handler/magnet"};
static const char *const cat_filemgr_mimes[] = {"inode/directory"};
static const char *const cat_archive_mimes[] = {
    "application/zip",     "application/x-tar",       "application/x-7z-compressed",
    "application/vnd.rar", "application/gzip", "application/x-bzip2",       "application/x-xz",
};
static const char *const cat_software_mimes[] = {"application/x-desktop"};
static const char *const cat_text_mimes[] = {"text/plain"};
static const char *const cat_pdf_mimes[] = {"application/pdf"};
static const char *const cat_doc_mimes[] = {
    "application/vnd.oasis.opendocument.text",
    "application/msword",
    "application/vnd.openxmlformats-officedocument.wordprocessingml.document",
    "text/rtf",
};
static const char *const cat_sheet_mimes[] = {
    "application/vnd.oasis.opendocument.spreadsheet",
    "application/vnd.ms-excel",
    "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet",
    "text/csv",
};
static const char *const cat_slide_mimes[] = {
    "application/vnd.oasis.opendocument.presentation",
    "application/vnd.ms-powerpoint",
    "application/vnd.openxmlformats-officedocument.presentationml.presentation",
};
static const char *const cat_ebook_mimes[] = {"application/epub+zip",
                                              "application/x-mobipocket-ebook"};
static const char *const cat_image_mimes[] = {
    "image/png", "image/jpeg", "image/gif", "image/bmp", "image/webp", "image/svg+xml",
    "image/tiff",
};
static const char *const cat_video_mimes[] = {
    "video/mp4",  "video/x-matroska", "video/webm",      "video/mpeg",
    "video/quicktime", "video/x-msvideo", "video/ogg",
};
static const char *const cat_audio_mimes[] = {
    "audio/mpeg", "audio/flac", "audio/x-flac", "audio/ogg",
    "audio/wav",  "audio/x-wav", "audio/aac",    "audio/opus",
};

#define DC_MIMES(arr) arr, (int)(sizeof(arr) / sizeof((arr)[0]))

static const dc_default_app_category DEFAULT_APP_CATEGORIES[] = {
    {"Web Browser", "INTERNET", DC_MIMES(cat_browser_mimes), "WebBrowser", true, false},
    {"Email", "INTERNET", DC_MIMES(cat_mailto_mimes), NULL, false, false},
    {"Calendar", "INTERNET", DC_MIMES(cat_calendar_mimes), NULL, false, false},
    {"RSS Reader", "INTERNET", DC_MIMES(cat_rss_mimes), NULL, false, false},
    {"Maps", "INTERNET", DC_MIMES(cat_geo_mimes), NULL, false, false},
    {"Torrent Client", "INTERNET", DC_MIMES(cat_torrent_mimes), NULL, false, false},
    {"File Manager", "FILES & SYSTEM", DC_MIMES(cat_filemgr_mimes), "FileManager", false, false},
    {"Terminal", "FILES & SYSTEM", NULL, 0, "TerminalEmulator", false, true},
    {"Archive Manager", "FILES & SYSTEM", DC_MIMES(cat_archive_mimes), NULL, false, false},
    {"Software Center", "FILES & SYSTEM", DC_MIMES(cat_software_mimes), NULL, false, false},
    {"Text Editor", "DOCUMENTS", DC_MIMES(cat_text_mimes), NULL, false, false},
    {"PDF Viewer", "DOCUMENTS", DC_MIMES(cat_pdf_mimes), NULL, false, false},
    {"Document Viewer", "DOCUMENTS", DC_MIMES(cat_doc_mimes), NULL, false, false},
    {"Spreadsheet", "DOCUMENTS", DC_MIMES(cat_sheet_mimes), NULL, false, false},
    {"Presentation", "DOCUMENTS", DC_MIMES(cat_slide_mimes), NULL, false, false},
    {"E-book Reader", "DOCUMENTS", DC_MIMES(cat_ebook_mimes), NULL, false, false},
    {"Image Viewer", "MEDIA", DC_MIMES(cat_image_mimes), NULL, false, false},
    {"Video Player", "MEDIA", DC_MIMES(cat_video_mimes), NULL, false, false},
    {"Music Player", "MEDIA", DC_MIMES(cat_audio_mimes), NULL, false, false},
};
#define DC_DEFAULT_APP_CAT_COUNT \
    (int)(sizeof(DEFAULT_APP_CATEGORIES) / sizeof(DEFAULT_APP_CATEGORIES[0]))

/* Per-row read cache for the generic MIME categories (mirrors
 * xdg_default_browser()/xdg_default_terminal()'s single-role statics above,
 * generalized to every row instead of one static per role). Indexed by the
 * row's position in DEFAULT_APP_CATEGORIES[]. */
typedef struct {
    char cache[DC_XDG_ID_MAX];
    bool cache_ok;
    time_t cache_time;
} dc_xdg_mime_cache;
static dc_xdg_mime_cache g_default_app_cache[32];

static bool xdg_default_mime(int idx, const char *mime, char *out, size_t n)
{
    dc_xdg_mime_cache *slot = &g_default_app_cache[idx];
    time_t now = time(NULL);
    if (slot->cache_time && now - slot->cache_time < SYS_CACHE_SECONDS) {
        snprintf(out, n, "%s", slot->cache);
        return slot->cache_ok;
    }
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "xdg-mime query default %s 2>/dev/null", mime);
    slot->cache_time = now;
    slot->cache_ok = xdg_query(cmd, slot->cache, sizeof(slot->cache));
    snprintf(out, n, "%s", slot->cache);
    return slot->cache_ok;
}

/* Batch every mime in `mimes` into one `xdg-mime default` call (one file
 * rewrite instead of N -- see docs/17 sec 1.1/4). */
static void xdg_set_mimes(const char *desktop_id, const char *const *mimes, int nmimes)
{
    char cmd[768];
    int off = snprintf(cmd, sizeof(cmd), "xdg-mime default %s", desktop_id);
    for (int i = 0; i < nmimes && off > 0 && (size_t)off < sizeof(cmd); i++)
        off += snprintf(cmd + off, sizeof(cmd) - (size_t)off, " %s", mimes[i]);
    run_xdg_detached(cmd);
}

/* One Default Apps row: current value (read-only, live xdg query, cached
 * SYS_CACHE_SECONDS) + a scrollable pick-list of installed apps that
 * genuinely declare the category's MimeType=/Categories= (dc_apps_find_by_
 * mime()/dc_apps_find_by_category(), apps.c -- real desktop-entry data, not
 * a name heuristic). Clicking a candidate writes it as the new default via
 * the row's write path: xdg-settings + a batched xdg-mime write for the
 * browser (docs/17 sec 4), xdg-terminals.list for the terminal, else one
 * batched `xdg-mime default` call covering every mime in the row. `idx` is
 * this row's position in DEFAULT_APP_CATEGORIES[], used only to key the
 * per-row read cache above. */
static void default_app_row(uictx *c, const dc_default_app_category *cat, int idx)
{
    char cur[DC_XDG_ID_MAX];
    bool have;
    if (cat->is_browser)
        have = xdg_default_browser(cur, sizeof(cur));
    else if (cat->is_terminal)
        have = xdg_default_terminal(cur, sizeof(cur));
    else
        have = xdg_default_mime(idx, cat->mimes[0], cur, sizeof(cur));

    char cur_stripped[DC_XDG_ID_MAX];
    if (have)
        strip_desktop_suffix(cur, cur_stripped, sizeof(cur_stripped));
    else
        cur_stripped[0] = '\0';

    char valline[64];
    snprintf(valline, sizeof(valline), "%s", have ? cur_stripped : "(none set)");
    ui_value(c, cat->label, valline);

    const dc_app *apps[16];
    int n = cat->category ? dc_apps_find_by_category(c->s->apps, cat->category, apps, 16)
                          : dc_apps_find_by_mime(c->s->apps, cat->mimes[0], apps, 16);

    int shown = 0;
    for (int i = 0; i < n && shown < 6; i++) {
        bool active = have && strcmp(apps[i]->id, cur_stripped) == 0;
        if (ui_list_row(c, apps[i]->name, active ? "Default" : NULL, 0, active) == 1 &&
            !active) {
            char withdesktop[DC_APP_ID + 16];
            snprintf(withdesktop, sizeof(withdesktop), "%s.desktop", apps[i]->id);
            if (cat->is_browser) {
                xdg_set_browser(withdesktop);
                xdg_set_mimes(withdesktop, cat->mimes, cat->nmimes);
            } else if (cat->is_terminal) {
                xdg_set_terminal(withdesktop);
            } else {
                xdg_set_mimes(withdesktop, cat->mimes, cat->nmimes);
            }
        }
        shown++;
    }
    if (shown == 0)
        ui_hint(c, "No apps found");
}

/* docs/17-DEFAULT-APPS-PLAN.md: 19-category default-apps manager. Reads are
 * always live (xdg-settings/xdg-mime/xdg-terminals.list, cached
 * SYS_CACHE_SECONDS); writes are detached shell commands gated by
 * DANKC_XDG_DRYRUN for offline verification (see run_xdg_detached() above)
 * -- this tab never has its own config.json keys, same pattern as the Audio/
 * Network/Bluetooth tabs (the xdg databases and $XDG_CONFIG_HOME/xdg-
 * terminals.list ARE the persisted state; dc_config_save() has nothing to
 * add). One generalized loop over DEFAULT_APP_CATEGORIES[] replaces the old
 * one-hand-written-block-per-role approach. */
static void tab_default_apps(uictx *c)
{
    if (!c->s->apps)
        c->s->apps = dc_apps_load();

    const char *last_section = NULL;
    for (int i = 0; i < DC_DEFAULT_APP_CAT_COUNT; i++) {
        const dc_default_app_category *cat = &DEFAULT_APP_CATEGORIES[i];
        if (!last_section || strcmp(last_section, cat->section) != 0) {
            ui_section(c, cat->section);
            last_section = cat->section;
        }
        default_app_row(c, cat, i);
    }
    ui_hint(c, "Terminal uses xdg-terminals.list (read by xdg-terminal-exec); Web");
    ui_hint(c, "Browser/File Manager/Terminal match Categories=, others match");
    ui_hint(c, "MimeType= (real .desktop data, not a name guess).");
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
/* One row of the SYSTEM THEMING section below: a toggle bound to *field,
 * plus a "not detected" hint when dc_systheme_app_detected(app_id) is false
 * (the app is opted in but dc_systheme_apply() will skip it until it's
 * actually installed), plus an optional reload caveat for apps that don't
 * live-reload their theme file. */
static void systheme_app_row(uictx *c, const char *label, const char *app_id, bool *field,
                             const char *caveat)
{
    if (ui_toggle(c, label, NULL, *field)) {
        *field = !*field;
        c->changed = true;
    }
    if (!dc_systheme_app_detected(app_id))
        ui_hint(c, "Not detected on this system");
    if (caveat)
        ui_hint(c, caveat);
}

/* docs/21-THEMING-COVERAGE-PLAN.md Task 8: the ~33 themable apps are grouped
 * into these fixed categories (Settings UI section, §3). One theme_app_entry
 * per app row; systheme_category() below renders a whole category at once. */
typedef struct {
    const char *label;
    const char *app_id;
    bool *field;
    const char *caveat;
} theme_app_entry;

/* Lightweight sub-header for one SYSTEM THEMING category -- deliberately
 * smaller/dimmer than ui_section() so the ~7 categories read as a grouping
 * within the existing section rather than as new top-level sections. */
static void ui_subsection(uictx *c, const char *label)
{
    c->y += 10.0f;
    if (c->mode == UI_RENDER) {
        nvgFontFaceId(c->vg, c->s->render->font_ui);
        nvgFontSize(c->vg, ui_fs(c, 11.0f));
        nvgTextAlign(c->vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgFillColor(c->vg, tc(c->t->surface_variant_text));
        nvgText(c->vg, 0, c->y + 6.0f, label, NULL);
    }
    c->y += 18.0f;
}

/* Renders one SYSTEM THEMING category: a sub-header, then detected-first app
 * rows -- undetected apps stay hidden behind a trailing "Show N undetected"
 * row until clicked. `open` is that category's session-local expand flag
 * (see the static bool array in tab_theme_colors(); never persisted to
 * config, resets every process restart). If every app in the category is
 * undetected, the header + affordance still render (no dead empty section). */
static void systheme_category(uictx *c, const char *title, const theme_app_entry *entries, int n,
                              bool *open)
{
    int hidden = 0;
    for (int i = 0; i < n; i++)
        if (!dc_systheme_app_detected(entries[i].app_id))
            hidden++;

    ui_subsection(c, title);
    for (int i = 0; i < n; i++) {
        if (!*open && !dc_systheme_app_detected(entries[i].app_id))
            continue;
        systheme_app_row(c, entries[i].label, entries[i].app_id, entries[i].field,
                        entries[i].caveat);
    }
    if (hidden > 0) {
        char label[48];
        snprintf(label, sizeof(label), "%s %d undetected", *open ? "Hide" : "Show", hidden);
        if (ui_list_row(c, label, NULL, *open ? IC_EXPAND_LESS : IC_EXPAND_MORE, false) == 1)
            *open = !*open;
    }
}

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

    /* docs/.../system-theming task 4: settings UI for services/systheme.c
     * (task 1, merged). Toggles here only flip dc_config fields + set
     * c->changed -- dc_config_reapply() -> apply_theme() -> dc_systheme_apply()
     * already runs on every settings change and is the sole writer of the
     * actual app theme files (see systheme.h). Nothing here touches disk. */
    ui_section(c, "SYSTEM THEMING");
    if (ui_toggle(c, "Theme system apps",
                  "Recolor GTK, Qt, terminals, editors, launchers & more from your theme",
                  c->cfg->systheme_enabled)) {
        c->cfg->systheme_enabled = !c->cfg->systheme_enabled;
        c->changed = true;
    }
    ui_hint(c, "Opt-in -- writes each app's own native theme file, backing up");
    ui_hint(c, "anything user-owned once before the first change.");
    if (c->cfg->systheme_enabled) {
        dc_config *g = c->cfg;
        /* Session-local "show undetected" expand flags, one per category
         * below -- plain function-static bools, never written to config.json,
         * reset to collapsed (false) on every process restart. */
        static bool open_toolkits, open_terminals, open_editors, open_launchers,
                    open_notifications, open_browsers, open_media;

        const theme_app_entry toolkits[] = {
            {"GTK", "gtk", &g->systheme_gtk,
             "Already-running GTK apps may need a restart to pick this up"},
            {"GTK 2", "gtk2", &g->systheme_gtk2,
             "Restart GTK2 apps for the ~/.gtkrc-2.0 change to apply"},
            {"Qt", "qt", &g->systheme_qt,
             "Needs QT_QPA_PLATFORMTHEME=qt5ct/6ct set; apply on restart"},
            {"Kvantum", "kvantum", &g->systheme_kvantum,
             "Needs Kvantum as the active Qt style; restart apps to apply"},
            {"KDE", "kde", &g->systheme_kde,
             "Live for most KDE apps; some need a restart"},
        };
        systheme_category(c, "TOOLKITS", toolkits, DC_ARRAY_LEN(toolkits), &open_toolkits);

        const theme_app_entry terminals[] = {
            {"Alacritty", "alacritty", &g->systheme_alacritty, NULL},
            {"Kitty", "kitty", &g->systheme_kitty, NULL},
            {"Foot", "foot", &g->systheme_foot, "Restart foot to apply"},
            {"Ghostty", "ghostty", &g->systheme_ghostty, NULL},
            {"WezTerm", "wezterm", &g->systheme_wezterm,
             "Add color_scheme = \"DankC\" to wezterm.lua once to activate"},
            {"Konsole", "konsole", &g->systheme_konsole,
             "Restart Konsole to apply the new color scheme"},
            {"Xresources", "xresources", &g->systheme_xresources,
             "New terminal instances only (xrdb -merge); XWayland apps only"},
        };
        systheme_category(c, "TERMINALS", terminals, DC_ARRAY_LEN(terminals), &open_terminals);

        const theme_app_entry editors[] = {
            {"VS Code", "vscode", &g->systheme_vscode,
             "Reopen VS Code windows to see the new colors"},
            {"Zed", "zed", &g->systheme_zed, "Reopen Zed to see the new theme"},
            {"Helix", "helix", &g->systheme_helix, NULL},
            {"Neovim", "neovim", &g->systheme_neovim,
             "Run :colorscheme dank once to activate"},
            {"Vim", "vim", &g->systheme_vim,
             "Add \":colorscheme dank\" to your vimrc once to activate"},
            {"Sublime Text", "sublime", &g->systheme_sublime, NULL},
            {"Emacs", "emacs", &g->systheme_emacs,
             "Add (load-theme 'dank) to your init file once to activate"},
        };
        systheme_category(c, "EDITORS", editors, DC_ARRAY_LEN(editors), &open_editors);

        const theme_app_entry launchers[] = {
            {"Rofi", "rofi", &g->systheme_rofi, NULL},
            {"Wofi", "wofi", &g->systheme_wofi, NULL},
            {"Fuzzel", "fuzzel", &g->systheme_fuzzel, NULL},
            {"Tofi", "tofi", &g->systheme_tofi, NULL},
        };
        systheme_category(c, "LAUNCHERS", launchers, DC_ARRAY_LEN(launchers), &open_launchers);

        const theme_app_entry notifications[] = {
            {"Mako", "mako", &g->systheme_mako, NULL},
            {"Dunst", "dunst", &g->systheme_dunst, NULL},
            {"SwayNC", "swaync", &g->systheme_swaync,
             "Only applied if swaync's style.css already exists"},
        };
        systheme_category(c, "NOTIFICATIONS", notifications, DC_ARRAY_LEN(notifications),
                          &open_notifications);

        const theme_app_entry browsers[] = {
            {"Firefox", "firefox", &g->systheme_firefox, "Chrome UI only; restart Firefox"},
            {"qutebrowser", "qutebrowser", &g->systheme_qutebrowser,
             "Only applied if qutebrowser's config.py already exists"},
            {"Discord", "discord", &g->systheme_discord,
             "Needs Vencord/Vesktop; enable theme in-client"},
        };
        systheme_category(c, "BROWSERS & CHAT", browsers, DC_ARRAY_LEN(browsers), &open_browsers);

        const theme_app_entry media[] = {
            {"btop", "btop", &g->systheme_btop, NULL},
            {"cava", "cava", &g->systheme_cava, NULL},
            {"Zathura", "zathura", &g->systheme_zathura, "Restart Zathura to apply"},
            {"Spicetify", "spicetify", &g->systheme_spicetify,
             "Needs spicetify CLI + backup"},
        };
        systheme_category(c, "MEDIA & MONITORS", media, DC_ARRAY_LEN(media), &open_media);
    }
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

/* ====================== Displays (docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.3) =====
 *
 * services/display.c owns the actual niri IPC (read via one-shot "Outputs"/
 * "FocusedOutput" socket requests, write via fire-and-forget `niri msg
 * output <name> ...`, gated by $DANKC_DISPLAY_DRYRUN for offline
 * verification -- see that file's header). Everything below is pure UI glue
 * calling those functions directly, same shape as the Audio/Network/
 * Bluetooth tabs (no config.json keys -- the live niri session + the
 * persisted KDL file ARE the state). Runtime actions apply instantly;
 * "Save as default" (bottom of the tab) is the only action that writes to
 * disk, via dc_display_persist().
 */

/* One deduplicated (width, height, refresh_mhz) option for the resolution
 * picker, with the modes[] index it corresponds to (dc_display_set_mode()
 * only needs the values, but the index lets us mark the current selection).
 * Real panels report duplicate entries at the same WxH@refresh (interlaced/
 * legacy CRTC variants niri surfaces separately) -- collapsing them here
 * keeps the on-screen list to what a user actually needs to choose between.
 */
typedef struct {
    int width, height, refresh_mhz;
    bool is_preferred;
    int mode_idx;
} disp_mode_opt;

static int disp_unique_modes(const dc_display_info *o, disp_mode_opt out[DC_DISPLAY_MAX_MODES])
{
    int n = 0;
    for (int i = 0; i < o->mode_count; i++) {
        bool dup = false;
        for (int j = 0; j < n; j++) {
            if (out[j].width == o->modes[i].width && out[j].height == o->modes[i].height &&
                out[j].refresh_mhz == o->modes[i].refresh_mhz) {
                dup = true;
                break;
            }
        }
        if (dup)
            continue;
        out[n].width = o->modes[i].width;
        out[n].height = o->modes[i].height;
        out[n].refresh_mhz = o->modes[i].refresh_mhz;
        out[n].is_preferred = o->modes[i].is_preferred;
        out[n].mode_idx = i;
        n++;
    }
    return n;
}

static void disp_mode_label(int width, int height, int refresh_mhz, bool preferred, char *buf,
                            size_t n)
{
    char refresh[16];
    dc_display_format_refresh(refresh_mhz, refresh, sizeof(refresh));
    snprintf(buf, n, "%dx%d @ %sHz%s", width, height, refresh, preferred ? "  (preferred)" : "");
}

/* Full control set for one monitor: resolution picker, scale presets,
 * rotation, position/arrangement, enable + VRR toggles. `outs`/`n` is the
 * full list (needed for the arrangement buttons, which place `idx` relative
 * to every *other* connected output); `idx` is the monitor these controls
 * currently act on. */
static void disp_monitor_detail(uictx *c, const dc_display_info *outs, int n, int idx)
{
    const dc_display_info *o = &outs[idx];

    ui_section(c, "RESOLUTION & REFRESH RATE");
    char cur_label[64];
    if (o->current_mode_idx >= 0 && o->current_mode_idx < o->mode_count)
        disp_mode_label(o->modes[o->current_mode_idx].width, o->modes[o->current_mode_idx].height,
                        o->modes[o->current_mode_idx].refresh_mhz, false, cur_label,
                        sizeof(cur_label));
    else
        snprintf(cur_label, sizeof(cur_label), "%dx%d (unknown mode)", o->logical_width,
                o->logical_height);
    if (ui_list_row(c, cur_label, o->mode_count > 0 ? "Change" : NULL,
                    c->s->disp_res_open ? IC_EXPAND_LESS : IC_EXPAND_MORE, false) == 1) {
        c->s->disp_res_open = !c->s->disp_res_open;
    }
    if (c->s->disp_res_open) {
        disp_mode_opt opts[DC_DISPLAY_MAX_MODES];
        int on = disp_unique_modes(o, opts);
        for (int i = 0; i < on; i++) {
            char label[64];
            disp_mode_label(opts[i].width, opts[i].height, opts[i].refresh_mhz,
                            opts[i].is_preferred, label, sizeof(label));
            bool active = opts[i].mode_idx == o->current_mode_idx;
            if (ui_list_row(c, label, NULL, 0, active) == 1 && !active) {
                dc_display_set_mode(o->name, opts[i].width, opts[i].height, opts[i].refresh_mhz);
                c->s->disp_res_open = false;
            }
        }
        if (on == 0)
            ui_hint(c, "No modes reported for this display");
    }

    ui_section(c, "SCALE");
    static const float presets[5] = {1.0f, 1.25f, 1.5f, 1.75f, 2.0f};
    static const char *const preset_labels[5] = {"100%", "125%", "150%", "175%", "200%"};
    int cur_preset = -1;
    for (int i = 0; i < 5; i++)
        if (fabsf((float)o->scale - presets[i]) < 0.01f)
            cur_preset = i;
    int scale_clicked = ui_segmented(c, "Display scale", preset_labels, 5, cur_preset);
    if (scale_clicked >= 0)
        dc_display_set_scale(o->name, (double)presets[scale_clicked]);
    if (cur_preset < 0) {
        char sv[32];
        snprintf(sv, sizeof(sv), "%.0f%%", o->scale * 100.0);
        ui_value(c, "Current scale (custom)", sv);
    }

    ui_section(c, "ORIENTATION");
    static const char *const rot_labels[4] = {"Normal", "90\xc2\xb0", "180\xc2\xb0", "270\xc2\xb0"};
    static const dc_display_transform rot_vals[4] = {
        DC_DISPLAY_TRANSFORM_NORMAL, DC_DISPLAY_TRANSFORM_90, DC_DISPLAY_TRANSFORM_180,
        DC_DISPLAY_TRANSFORM_270};
    int cur_rot = -1;
    for (int i = 0; i < 4; i++)
        if (rot_vals[i] == o->transform)
            cur_rot = i;
    int rot_clicked = ui_segmented(c, "Rotation", rot_labels, 4, cur_rot);
    if (rot_clicked >= 0)
        dc_display_set_transform(o->name, rot_vals[rot_clicked]);
    if (cur_rot < 0)
        ui_hint(c, "Currently flipped -- pick a rotation above to clear the flip");

    ui_section(c, "POSITION & ARRANGEMENT");
    char posv[32];
    snprintf(posv, sizeof(posv), "%d, %d", o->x, o->y);
    ui_value(c, "Position (x, y)", posv);

    /* Relative-placement buttons against every other connected output --
     * the cheapest correct "arrangement" UI without a drag canvas (no such
     * widget primitive exists in the shared kit above; docs/19 sec.3
     * explicitly prefers this over a mini-canvas for that reason). Logical
     * (post-scale) sizes, since niri's x/y position space is logical. */
    for (int j = 0; j < n; j++) {
        if (j == idx)
            continue;
        const dc_display_info *other = &outs[j];
        char rel[96];
        snprintf(rel, sizeof(rel), "Place relative to %s:", other->name);
        ui_hint(c, rel);
        static const char *const dir_labels[4] = {"Left of", "Right of", "Above", "Below"};
        int dir = ui_segmented(c, "Direction", dir_labels, 4, -1);
        if (dir >= 0) {
            int nx = other->x, ny = other->y;
            switch (dir) {
            case 0:
                nx = other->x - o->logical_width;
                break;
            case 1:
                nx = other->x + other->logical_width;
                break;
            case 2:
                ny = other->y - o->logical_height;
                break;
            case 3:
                ny = other->y + other->logical_height;
                break;
            }
            dc_display_set_position(o->name, nx, ny);
        }
    }

    int xv = o->x;
    if (ui_stepper(c, "Fine-tune X", &xv, -10000, 10000, 10))
        dc_display_set_position(o->name, xv, o->y);
    int yv = o->y;
    if (ui_stepper(c, "Fine-tune Y", &yv, -10000, 10000, 10))
        dc_display_set_position(o->name, o->x, yv);
    if (ui_list_row(c, "Let niri auto-arrange this display", NULL, 0, false) == 1)
        dc_display_set_position_auto(o->name);

    ui_section(c, "POWER & VRR");
    int enabled_count = 0;
    for (int i = 0; i < n; i++)
        if (outs[i].enabled)
            enabled_count++;
    bool can_disable = !o->enabled || enabled_count > 1;
    if (ui_toggle(c, "Enabled",
                 can_disable ? "Turn this display off" : "Can't disable the only active display",
                 o->enabled)) {
        if (can_disable)
            dc_display_set_enabled(o->name, !o->enabled);
    }
    if (o->vrr_supported) {
        if (ui_toggle(c, "Variable refresh rate", "Reduce tearing at variable frame rates",
                     o->vrr_enabled))
            dc_display_set_vrr(o->name, !o->vrr_enabled);
    } else {
        ui_hint(c, "Variable refresh rate not supported on this display");
    }
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

    ui_section(c, "MONITORS");
    dc_display_info outs[DC_DISPLAY_MAX_OUTPUTS];
    int n = dc_display_list(outs);
    if (n == 0) {
        ui_hint(c, "No outputs reported (niri IPC unavailable?)");
        return;
    }
    if (c->s->disp_selected >= n)
        c->s->disp_selected = 0;

    for (int i = 0; i < n; i++) {
        const dc_display_info *o = &outs[i];
        int rw = (o->current_mode_idx >= 0 && o->current_mode_idx < o->mode_count)
                        ? o->modes[o->current_mode_idx].width
                        : o->logical_width;
        int rh = (o->current_mode_idx >= 0 && o->current_mode_idx < o->mode_count)
                        ? o->modes[o->current_mode_idx].height
                        : o->logical_height;
        char title[96];
        snprintf(title, sizeof(title), "%s%s", o->name, o->is_focused ? "  (focused)" : "");
        char status[64];
        snprintf(status, sizeof(status), "%dx%d @ %d%%%s", rw, rh,
                (int)lroundf((float)o->scale * 100.0f), o->enabled ? "" : "  \xe2\x80\xa2 off");
        bool active = i == c->s->disp_selected;
        if (ui_list_row(c, title, status, 0, active) == 1 && !active) {
            c->s->disp_selected = i;
            c->s->disp_res_open = false;
        }
        if (o->make[0] || o->model[0]) {
            char mm[160];
            snprintf(mm, sizeof(mm), "%s %s", o->make, o->model);
            ui_hint(c, mm);
        }
    }

    disp_monitor_detail(c, outs, n, c->s->disp_selected);

    ui_section(c, "SAVE");
    ui_hint(c, "Changes above apply immediately for this session (niri msg output).");
    ui_hint(c, "They're forgotten when niri restarts unless saved as default below.");
    if (ui_list_row(c, "Save as default (write to niri config)", NULL, IC_DONE, false) == 1) {
        dc_display_persist_config cfgs[DC_DISPLAY_MAX_OUTPUTS];
        int cn = 0;
        for (int i = 0; i < n; i++) {
            const dc_display_info *o = &outs[i];
            dc_display_persist_config *pc = &cfgs[cn++];
            memset(pc, 0, sizeof(*pc));
            snprintf(pc->name, sizeof(pc->name), "%s", o->name);
            if (o->current_mode_idx >= 0 && o->current_mode_idx < o->mode_count) {
                pc->has_mode = true;
                pc->width = o->modes[o->current_mode_idx].width;
                pc->height = o->modes[o->current_mode_idx].height;
                pc->refresh_mhz = o->modes[o->current_mode_idx].refresh_mhz;
            }
            pc->has_scale = true;
            pc->scale = o->scale;
            pc->has_transform = true;
            pc->transform = o->transform;
            pc->has_position = true;
            pc->x = o->x;
            pc->y = o->y;
            pc->has_enabled = true;
            pc->enabled = o->enabled;
            if (o->vrr_supported) {
                pc->has_vrr = true;
                pc->vrr_enabled = o->vrr_enabled;
            }
        }
        dc_display_persist(cfgs, cn, NULL);
    }
}

/* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.4: Night Light -- its own tab now
 * (replaces the old duplicated pgrep/pkill/gammastep-O-4000 one-shot that
 * used to live in tab_displays; main.c's `ctl night` was already migrated to
 * this same services/nightlight.c backend). All state is owned by
 * nightlight.c (persists to config.json itself) -- this tab is a thin
 * getter/setter view, same shape as tab_power over services/power.c. */
static void tab_nightlight(uictx *c)
{
    ui_section(c, "NIGHT LIGHT");
    dc_nightlight_backend be = dc_nightlight_backend_get();
    if (be == DC_NIGHTLIGHT_BACKEND_NONE) {
        ui_hint(c, "No backend found -- install wlsunset or gammastep.");
        return;
    }
    char backend_line[64];
    snprintf(backend_line, sizeof(backend_line), "Backend: %s", dc_nightlight_backend_name(be));
    ui_hint(c, backend_line);

    static opt_flip enabled_flip;
    bool enabled = flip_get(&enabled_flip, dc_nightlight_active());
    if (ui_toggle(c, "Enable Night Light", "Warm color temperature at night", enabled)) {
        dc_nightlight_enable(!enabled);
        flip_set(&enabled_flip, !enabled);
    }

    ui_section(c, "TEMPERATURE");
    static opt_value temp_pending;
    float temp = (float)dc_nightlight_get_temp();
    float pv;
    if (opt_value_get(&temp_pending, &pv))
        temp = pv;
    char tv[16];
    snprintf(tv, sizeof(tv), "%dK", (int)lroundf(temp));
    if (ui_slider(c, "Color temperature (night side)", &temp, 2500.0f, 6500.0f, tv)) {
        int k = (int)lroundf(temp);
        dc_nightlight_set_temp(k);
        opt_value_set(&temp_pending, (float)k);
    }

    ui_section(c, "SCHEDULE");
    dc_nightlight_schedule sched = dc_nightlight_get_schedule();
    static const char *const sched_opts[] = {"Manual", "Sunset - Sunrise", "Custom Times"};
    int clicked = ui_segmented(c, "Mode", sched_opts, 3, (int)sched);
    if (clicked >= 0 && clicked != (int)sched) {
        char from[6], to[6];
        dc_nightlight_get_times(from, sizeof(from), to, sizeof(to));
        dc_nightlight_set_schedule((dc_nightlight_schedule)clicked, from, to);
        sched = (dc_nightlight_schedule)clicked;
    }

    if (sched == DC_NIGHTLIGHT_SCHED_SUNSET) {
        ui_hint(c, "Uses the Weather tab's latitude/longitude for sunset/sunrise.");
    } else if (sched == DC_NIGHTLIGHT_SCHED_TIMES) {
        char from[6], to[6];
        dc_nightlight_get_times(from, sizeof(from), to, sizeof(to));

        bool from_focus = c->s->focus_field == 7;
        char frombuf[6];
        if (from_focus)
            copy_trunc(frombuf, sizeof(frombuf), c->s->edit_buf);
        else
            snprintf(frombuf, sizeof(frombuf), "%s", from);
        if (ui_textfield(c, "From (HH:MM)", frombuf, from_focus)) {
            c->s->focus_field = 7;
            snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%s", from);
        }

        bool to_focus = c->s->focus_field == 8;
        char tobuf[6];
        if (to_focus)
            copy_trunc(tobuf, sizeof(tobuf), c->s->edit_buf);
        else
            snprintf(tobuf, sizeof(tobuf), "%s", to);
        if (ui_textfield(c, "To (HH:MM)", tobuf, to_focus)) {
            c->s->focus_field = 8;
            snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%s", to);
        }
    }
}

/* docs/25-AUDIO-PERDEVICE-PLAN.md T4: per-device cards, replacing the old
 * single-default-sink slider + fragile `wpctl status`-parsed sink list (both
 * deleted -- services/audio.c now owns enumeration + the max-volume clamp +
 * alias resolution via the dc_audio_sinks()/sources()/dc_audio_device_*()/
 * dc_audio_display_name() API, config.c owns persistence via
 * dc_config_audio_max()/_alias()/_hidden() + setters). Up to
 * AUDIO_CARD_DEVICES_MAX sinks/sources are listed per category -- generously
 * above what any real machine reports, matching the service's own
 * DC_CONFIG_AUDIO_DEVICES_MAX-style headroom rather than the old SINKS_MAX==6
 * `wpctl status`-parser limit. */
#define AUDIO_CARD_DEVICES_MAX 16

/* Collapsed-row status text: "Default \xc2\xb7 42%", "Muted", "Default \xc2\xb7 Muted", or
 * plain "42%" for a non-default, unmuted device. */
static void audio_card_status(const dc_audio_device *dev, char *out, size_t n)
{
    size_t p = 0;
    if (dev->is_default) {
        int w = snprintf(out, n, "Default");
        p = w > 0 ? (size_t)w : 0;
    }
    if (p >= n)
        return;
    if (dev->muted)
        snprintf(out + p, n - p, p ? " \xc2\xb7 Muted" : "Muted");
    else
        snprintf(out + p, n - p, p ? " \xc2\xb7 %d%%" : "%d%%", dev->volume);
}

/* One OUTPUT/INPUT card: collapsed row (name + Default badge + vol/mute,
 * clicking sets default) with a chevron that expands full controls for
 * exactly one device per category at a time -- `expanded` is the caller's
 * (static, node.name-keyed) disclosure state, docs/25 T4's "expand ONE device
 * at a time" (D2: node.name is the stable key, not the session-local id).
 * `slot` (0 output-category / 1 input-category) selects which half of the
 * per-category optimistic-state statics below this card uses, so an expanded
 * output card and an expanded input card never share state. */
static void audio_device_card(uictx *c, const dc_audio_device *dev, int slot, char *expanded,
                              size_t expanded_sz)
{
    char status[48];
    audio_card_status(dev, status, sizeof(status));
    bool expanded_now = expanded[0] && strcmp(expanded, dev->name) == 0;
    int click = ui_list_row(c, dc_audio_display_name(dev), status,
                            expanded_now ? IC_EXPAND_LESS : IC_EXPAND_MORE, dev->is_default);
    if (click == 1) {
        if (!dev->is_default)
            dc_audio_set_default(dev->id);
    } else if (click == 2) {
        if (expanded_now)
            expanded[0] = '\0';
        else
            copy_trunc(expanded, expanded_sz, dev->name);
    }
    if (!expanded_now)
        return;

    static opt_value vol_pending[2];
    static opt_flip mute_flip[2];
    static opt_value max_pending[2];

    int max_vol = dc_config_audio_max(dev->name);
    float frac = (float)dev->volume / (float)max_vol;
    float pv;
    if (opt_value_get(&vol_pending[slot], &pv))
        frac = pv;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    char vv[16];
    snprintf(vv, sizeof(vv), "%d%%", (int)lroundf(frac * (float)max_vol));
    if (ui_slider(c, "Volume", &frac, 0.0f, 1.0f, vv)) {
        int pct = (int)lroundf(frac * (float)max_vol);
        dc_audio_device_set_volume(dev->id, pct);
        opt_value_set(&vol_pending[slot], frac);
    }

    bool muted = flip_get(&mute_flip[slot], dev->muted);
    if (ui_toggle(c, "Mute", NULL, muted)) {
        dc_audio_device_toggle_mute(dev->id);
        flip_set(&mute_flip[slot], !muted);
    }

    /* Max-volume clamp (D4): 100-200% slider, one entry per device in
     * config.json (dc_config_audio_set_max()); lowering it below the
     * device's current live volume corrects that volume immediately instead
     * of waiting for the service's own parse-time clamp to catch up. */
    float maxfrac = ((float)max_vol - 100.0f) / 100.0f;
    float mpv;
    if (opt_value_get(&max_pending[slot], &mpv))
        maxfrac = mpv;
    int curmax = 100 + (int)lroundf(maxfrac * 100.0f);
    char mv[16];
    snprintf(mv, sizeof(mv), "%d%%", curmax);
    if (ui_slider(c, "Max volume", &maxfrac, 0.0f, 1.0f, mv)) {
        int newmax = 100 + (int)lroundf(maxfrac * 100.0f);
        dc_config_audio_set_max(dev->name, newmax);
        c->changed = true;
        opt_value_set(&max_pending[slot], maxfrac);
        if (dev->volume > newmax)
            dc_audio_device_set_volume(dev->id, newmax);
    }

    /* Rename (D3: dankc-config-only alias, dc_audio_display_name() is what
     * every surface reads back) -- shares focus_field 15 with every other
     * device's rename field; s->audio_rename_target records which node.name
     * the in-progress edit_buf belongs to (set when this field gains focus,
     * same shape as the hotspot SSID/password drafts above). */
    bool renaming =
        c->s->focus_field == 15 && strcmp(c->s->audio_rename_target, dev->name) == 0;
    const char *alias = dc_config_audio_alias(dev->name);
    char namebuf[DC_CONFIG_AUDIO_ALIAS_MAX];
    if (renaming)
        copy_trunc(namebuf, sizeof(namebuf), c->s->edit_buf);
    else
        snprintf(namebuf, sizeof(namebuf), "%s", alias ? alias : "");
    if (ui_textfield(c, "Rename (blank = default name)", namebuf, renaming)) {
        c->s->focus_field = 15;
        copy_trunc(c->s->audio_rename_target, sizeof(c->s->audio_rename_target), dev->name);
        snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%s", alias ? alias : "");
    }

    /* Hiding the current default would strand playback/capture with no
     * visible way to switch away from it -- withheld for that one device,
     * matching docs/25 T4's "skip for current default". */
    if (!dev->is_default && ui_list_row(c, "Hide device", NULL, 0, false) == 1) {
        dc_config_audio_set_hidden(dev->name, true);
        c->changed = true;
        expanded[0] = '\0'; /* it's leaving this list -- nothing left to show expanded */
    }
}

static void tab_audio(uictx *c)
{
    dc_audio_device sinks[AUDIO_CARD_DEVICES_MAX];
    int sink_n = dc_audio_sinks(sinks, AUDIO_CARD_DEVICES_MAX);
    dc_audio_device sources[AUDIO_CARD_DEVICES_MAX];
    int source_n = dc_audio_sources(sources, AUDIO_CARD_DEVICES_MAX);

    static char expanded_sink[DC_CONFIG_AUDIO_NAME_MAX] = "";
    static char expanded_source[DC_CONFIG_AUDIO_NAME_MAX] = "";

    ui_section(c, "OUTPUT DEVICES");
    int out_shown = 0;
    for (int i = 0; i < sink_n; i++) {
        if (dc_config_audio_hidden(sinks[i].name))
            continue;
        out_shown++;
        audio_device_card(c, &sinks[i], 0, expanded_sink, sizeof(expanded_sink));
    }
    if (out_shown == 0)
        ui_hint(c, sink_n == 0 ? "No output devices found"
                               : "All output devices are hidden -- see Hidden Devices below");

    ui_section(c, "INPUT DEVICES");
    int in_shown = 0;
    for (int i = 0; i < source_n; i++) {
        if (dc_config_audio_hidden(sources[i].name))
            continue;
        in_shown++;
        audio_device_card(c, &sources[i], 1, expanded_source, sizeof(expanded_source));
    }
    if (in_shown == 0)
        ui_hint(c, source_n == 0 ? "No input devices found"
                                 : "All input devices are hidden -- see Hidden Devices below");

    /* Hidden section (D6: the service enumerates hidden devices too --
     * filtering only happens here in the UI -- so "Unhide" always has a
     * live device to act on rather than a name orphaned in config.json). */
    bool any_hidden = false;
    for (int i = 0; i < sink_n && !any_hidden; i++)
        any_hidden = dc_config_audio_hidden(sinks[i].name);
    for (int i = 0; i < source_n && !any_hidden; i++)
        any_hidden = dc_config_audio_hidden(sources[i].name);
    if (any_hidden) {
        ui_section(c, "HIDDEN DEVICES");
        for (int i = 0; i < sink_n; i++) {
            if (!dc_config_audio_hidden(sinks[i].name))
                continue;
            if (ui_list_row(c, dc_audio_display_name(&sinks[i]), "Output", IC_ADD, false) == 2) {
                dc_config_audio_set_hidden(sinks[i].name, false);
                c->changed = true;
            }
        }
        for (int i = 0; i < source_n; i++) {
            if (!dc_config_audio_hidden(sources[i].name))
                continue;
            if (ui_list_row(c, dc_audio_display_name(&sources[i]), "Input", IC_ADD, false) == 2) {
                dc_config_audio_set_hidden(sources[i].name, false);
                c->changed = true;
            }
        }
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

    /* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.2 extension: wired device,
     * hotspot, and saved-network management, all native NetworkManager D-Bus
     * (services/net.h) -- no nmcli popen()s on this path. */
    ui_section(c, "ETHERNET");
    dc_net_eth_info eth;
    if (dc_net_ethernet(&eth) && eth.has_device) {
        static const char *const eth_state_name[] = {"Unavailable", "No cable", "Disconnected",
                                                     "Connecting\xe2\x80\xa6", "Connected"};
        int si = (int)eth.state;
        ui_value(c, "Device", eth.device_name);
        ui_value(c, "State", eth_state_name[si >= 0 && si < 5 ? si : 0]);
        if (eth.connection_name[0])
            ui_value(c, "Connection", eth.connection_name);
        if (eth.state == DC_NET_ETH_CONNECTED) {
            char ipbuf[96];
            snprintf(ipbuf, sizeof(ipbuf), "%s/%u", eth.ipv4_address, eth.ipv4_prefix);
            ui_value(c, "IPv4 address", ipbuf);
            ui_value(c, "Gateway", eth.ipv4_gateway[0] ? eth.ipv4_gateway : "-");
            if (eth.ipv4_dns_count > 0) {
                char dnsbuf[3 * 64] = "";
                for (int i = 0; i < eth.ipv4_dns_count; i++) {
                    if (i > 0)
                        strncat(dnsbuf, ", ", sizeof(dnsbuf) - strlen(dnsbuf) - 1);
                    strncat(dnsbuf, eth.ipv4_dns[i], sizeof(dnsbuf) - strlen(dnsbuf) - 1);
                }
                ui_value(c, "DNS", dnsbuf);
            }
            if (eth.link_speed_mbps > 0) {
                char sp[32];
                snprintf(sp, sizeof(sp), "%u Mbps", eth.link_speed_mbps);
                ui_value(c, "Link speed", sp);
            }
        }
        if (eth.mac[0])
            ui_value(c, "MAC address", eth.mac);

        bool can_connect = eth.state == DC_NET_ETH_DISCONNECTED;
        bool can_disconnect =
            eth.state == DC_NET_ETH_CONNECTED || eth.state == DC_NET_ETH_CONNECTING;
        if (can_connect && ui_list_row(c, "Connect", NULL, IC_LINK, false) == 1)
            dc_net_eth_connect();
        if (can_disconnect && ui_list_row(c, "Disconnect", NULL, IC_REMOVE, false) == 1)
            dc_net_eth_disconnect();
    } else {
        ui_hint(c, "No ethernet device found");
    }

    ui_section(c, "HOTSPOT");
    char hs_ssid[64] = {0};
    bool hs_active = dc_net_hotspot_active(hs_ssid, sizeof(hs_ssid));
    static opt_flip hotspot_flip;
    bool hs_on = flip_get(&hotspot_flip, hs_active);
    if (ui_toggle(c, "Wi-Fi Hotspot", "Share your connection over Wi-Fi (access-point mode)",
                 hs_on)) {
        if (hs_on) {
            dc_net_hotspot_stop();
        } else {
            const char *ssid =
                c->s->net_hotspot_ssid[0] ? c->s->net_hotspot_ssid : "dankc-hotspot";
            dc_net_hotspot_start(ssid, c->s->net_hotspot_password[0] ? c->s->net_hotspot_password
                                                                     : NULL,
                                 NULL);
        }
        flip_set(&hotspot_flip, !hs_on);
    }
    ui_hint(c, "A single Wi-Fi radio can't be a client and a hotspot at once --");
    ui_hint(c, "starting this drops any current Wi-Fi client connection.");
    if (hs_active) {
        ui_value(c, "Active SSID", hs_ssid);
    } else {
        bool ssid_focus = c->s->focus_field == 9;
        char ssidbuf[64];
        if (ssid_focus)
            copy_trunc(ssidbuf, sizeof(ssidbuf), c->s->edit_buf);
        else
            snprintf(ssidbuf, sizeof(ssidbuf), "%s", c->s->net_hotspot_ssid);
        if (ui_textfield(c, "Hotspot name (SSID)", ssidbuf, ssid_focus)) {
            c->s->focus_field = 9;
            snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%s", c->s->net_hotspot_ssid);
        }

        bool pw_focus = c->s->focus_field == 10;
        char pw_source[64];
        if (pw_focus)
            copy_trunc(pw_source, sizeof(pw_source), c->s->edit_buf);
        else
            snprintf(pw_source, sizeof(pw_source), "%s", c->s->net_hotspot_password);
        /* Masked display -- dots for whatever's typed so far, same length as
         * the real value (no plaintext echo, matches a normal password
         * field's UX; this tab has no other masked-field precedent to
         * follow so the mask is built ad hoc here rather than in ui_textfield
         * itself). */
        char pwdisplay[64];
        size_t pn = strlen(pw_source);
        if (pn >= sizeof(pwdisplay))
            pn = sizeof(pwdisplay) - 1;
        memset(pwdisplay, '*', pn);
        pwdisplay[pn] = '\0';
        if (ui_textfield(c, "Password (blank = open network)", pwdisplay, pw_focus)) {
            c->s->focus_field = 10;
            snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%s", c->s->net_hotspot_password);
        }
    }

    ui_section(c, "SAVED WI-FI NETWORKS");
    dc_net_saved_net saved[DC_NET_SAVED_MAX];
    int sn = dc_net_saved_list(saved, DC_NET_SAVED_MAX);
    if (sn == 0) {
        ui_hint(c, "No saved networks");
    } else {
        for (int i = 0; i < sn; i++) {
            const dc_net_saved_net *nw = &saved[i];
            int clicked = ui_list_row(c, nw->id[0] ? nw->id : nw->ssid,
                                      nw->autoconnect ? "Auto-connect" : "Manual", IC_REMOVE,
                                      false);
            if (clicked == 2)
                dc_net_saved_forget(nw->path);
            else if (clicked == 1)
                dc_net_saved_set_autoconnect(nw->path, !nw->autoconnect);
        }
        ui_hint(c, "Click a network to toggle auto-connect, or the icon to forget it.");
    }

    ui_section(c, "VPN");
    dc_net_vpn vpns[DC_NET_VPN_MAX];
    int vn = dc_net_vpn_list(vpns, DC_NET_VPN_MAX);
    /* Per-profile optimistic flip (mirrors the HOTSPOT toggle above): NM's
     * ActivateConnection/DeactivateConnection are async, so the very next
     * dc_net_vpn_list() call can still report the pre-click state for a
     * second or two. Keyed by list index rather than path -- NM's
     * ListConnections order is stable across activation/deactivation (it
     * enumerates saved Settings.Connections, not ActiveConnections), so
     * indices don't shuffle between frames the way, say, a sorted-by-active
     * list would. */
    static opt_flip vpn_flips[DC_NET_VPN_MAX];
    if (vn == 0) {
        ui_hint(c, "No VPN profiles -- import with nmcli or nm-connection-editor");
    } else {
        for (int i = 0; i < vn; i++) {
            const dc_net_vpn *v = &vpns[i];
            bool active = flip_get(&vpn_flips[i], v->active);
            char status[32];
            snprintf(status, sizeof(status), "%s%s", v->type_is_wireguard ? "WireGuard" : "OpenVPN",
                     active ? " \xc2\xb7 Connected" : "");
            if (ui_list_row(c, v->id[0] ? v->id : v->path, status, 0, active) == 1) {
                if (active)
                    dc_net_vpn_deactivate(v->active_conn_path[0] ? v->active_conn_path : v->path);
                else
                    dc_net_vpn_activate(v->path);
                flip_set(&vpn_flips[i], !active);
            }
        }
    }
    ui_hint(c, "Click a profile to connect or disconnect. Import new profiles with");
    ui_hint(c, "`nmcli connection import` or nm-connection-editor -- dankc only");
    ui_hint(c, "manages saved profiles, not secrets/credentials (v1).");
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

/* Profile-mode labels shared by the POWER PROFILE segmented control and the
 * AUTOMATION section's per-power-source pickers below -- same 3 slugs
 * dc_power_mode enumerates (services/power.h) and the same plain ints
 * config.h's profile_on_ac/profile_on_battery use (see that struct's comment
 * for why those fields are ints rather than dc_power_mode). */
static const char *const dc_power_profile_opts[3] = {"Power Saver", "Balanced", "Performance"};

static void tab_power(uictx *c)
{
    ui_section(c, "POWER PROFILE");
    dc_power_info pw;
    if (!dc_power_read(&pw)) {
        ui_hint(c, "No power-profile backend (power-profiles-daemon or tuned) detected");
    } else {
        int cur = (int)pw.active_mode; /* -1 (unknown) selects nothing */
        int clicked = ui_segmented(c, "Profile", dc_power_profile_opts, 3, cur);
        if (clicked >= 0 && clicked != cur)
            dc_power_set_mode((dc_power_mode)clicked);
        /* Raw backend profile as a caption when it isn't literally one of the
         * 3 mode slugs (same rule as the battery popout's caption). */
        if (pw.active_profile[0])
            ui_value(c, "Active profile", pw.active_profile);
        ui_hint(c, "Lock, suspend and power-off live in the power menu (bar \xc2\xb7 power button)");
    }

    /* docs/24-BATTERY-POWER-PLAN.md T6: charge-limit protection plus the
     * notification/threshold/auto-power-saver knobs, all backed by config.h's
     * battery_* keys (T2) and battery.c's charge-limit read/write (T1/T3).
     * The whole section is skipped when no battery is present at all (a
     * desktop) -- matches every other tab's "unavailable" hint convention
     * (see POWER PROFILE / IDLE & LID above). Within it, the charge-limit
     * stepper specifically is further gated on charge_limit_supported since
     * plenty of drivers (ThinkPad/ASUS/LG EC-dependent) don't expose the
     * sysfs attribute at all -- the notif/threshold/auto-power-saver rows
     * don't need that support and stay available whenever a battery exists. */
    ui_section(c, "BATTERY");
    dc_battery_info bi;
    if (!dc_battery_read(&bi)) {
        ui_hint(c, "No battery detected");
    } else {
        if (bi.charge_limit_supported) {
            if (ui_stepper(c, "Charge limit (%)", &c->cfg->charge_limit, 50, 100, 5)) {
                c->changed = true;
                /* Fire-and-forget pkexec write (DANKC_BATTERY_DRYRUN-gated
                 * inside battery.c itself); UI reflects sysfs via poll-back
                 * below, not the wish, since there's no success signal. */
                dc_battery_set_charge_limit(bi.batt_dir, c->cfg->charge_limit);
            }
            ui_hint(c, "Caps charging to protect long-term battery health (100 = no limit)");
            if (bi.charge_limit != c->cfg->charge_limit) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Sysfs is at %d%% -- use the stepper to reapply %d%%",
                         bi.charge_limit, c->cfg->charge_limit);
                ui_hint(c, msg);
            }
            ui_hint(c, "The kernel resets this on every reboot; dankc never re-applies it at login.");
        } else {
            ui_hint(c, "This battery's driver doesn't expose a charge-limit control");
        }

        if (ui_toggle(c, "Battery notifications", "Low / critical / charge-limit-reached alerts",
                      c->cfg->battery_notifications)) {
            c->cfg->battery_notifications = !c->cfg->battery_notifications;
            c->changed = true;
        }
        if (ui_stepper(c, "Low battery threshold (%)", &c->cfg->low_battery_threshold, 1, 99, 5))
            c->changed = true;
        if (ui_stepper(c, "Critical battery threshold (%)", &c->cfg->critical_battery_threshold, 1,
                      99, 5))
            c->changed = true;
        if (ui_toggle(c, "Auto power saver",
                      "Switch to the Power Saver profile once battery hits the low threshold",
                      c->cfg->auto_power_saver)) {
            c->cfg->auto_power_saver = !c->cfg->auto_power_saver;
            c->changed = true;
        }
    }

    ui_section(c, "AUTOMATION");
    if (ui_toggle(c, "Automatic profile switching",
                 "Apply a chosen profile on every AC plug/unplug edge",
                 c->cfg->auto_profile_switch)) {
        c->cfg->auto_profile_switch = !c->cfg->auto_profile_switch;
        c->changed = true;
    }
    if (c->cfg->auto_profile_switch) {
        int ac_clicked =
            ui_segmented(c, "Profile on AC", dc_power_profile_opts, 3, c->cfg->profile_on_ac);
        if (ac_clicked >= 0 && ac_clicked != c->cfg->profile_on_ac) {
            c->cfg->profile_on_ac = ac_clicked;
            c->changed = true;
        }
        int bat_clicked = ui_segmented(c, "Profile on battery", dc_power_profile_opts, 3,
                                       c->cfg->profile_on_battery);
        if (bat_clicked >= 0 && bat_clicked != c->cfg->profile_on_battery) {
            c->cfg->profile_on_battery = bat_clicked;
            c->changed = true;
        }
    }

    /* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.9: Idle & Lid, a thin view
     * over services/logind.c's /etc/systemd/logind.conf (+ drop-ins) reader/
     * writer. Editing writes a dankc-owned drop-in via pkexec (never the
     * main logind.conf); dankc never restarts systemd-logind itself, so the
     * hint below tells the user a re-login is needed. */
    ui_section(c, "IDLE & LID");
    dc_logind_conf_info lc;
    dc_logind_conf_read(&lc, NULL);
    if (lc.from_dropin)
        ui_hint(c, "Some of these are overridden by an existing logind.conf.d drop-in.");

    static const char *const idle_opts[5] = {"Ignore", "Lock", "Suspend", "Hibernate", "Poweroff"};
    static const char *const idle_vals[5] = {"ignore", "lock", "suspend", "hibernate", "poweroff"};
    int idle_cur = -1;
    for (int i = 0; i < 5; i++)
        if (strcmp(lc.idle_action, idle_vals[i]) == 0)
            idle_cur = i;
    int idle_clicked = ui_segmented(c, "When idle", idle_opts, 5, idle_cur);
    if (idle_clicked >= 0 && idle_clicked != idle_cur) {
        snprintf(lc.idle_action, sizeof(lc.idle_action), "%s", idle_vals[idle_clicked]);
        dc_logind_conf_write_dropin(&lc);
    }

    int idle_min = (lc.idle_action_sec + 59) / 60;
    if (ui_stepper(c, "Idle timeout (minutes)", &idle_min, 1, 180, 1)) {
        lc.idle_action_sec = idle_min * 60;
        dc_logind_conf_write_dropin(&lc);
    }

    /* Lid-close action is per-power-source in logind (HandleLidSwitch vs.
     * HandleLidSwitchExternalPower) -- the writer has always accepted both
     * (logind.c) but this tab only ever exposed the first. Same opts/vals
     * list for both rows since systemd accepts identical values for each. */
    static const char *const lid_opts[5] = {"Ignore", "Lock", "Suspend", "Hibernate", "Poweroff"};
    static const char *const lid_vals[5] = {"ignore", "lock", "suspend", "hibernate", "poweroff"};
    int lid_cur = -1;
    for (int i = 0; i < 5; i++)
        if (strcmp(lc.handle_lid_switch, lid_vals[i]) == 0)
            lid_cur = i;
    int lid_clicked = ui_segmented(c, "Lid close action", lid_opts, 5, lid_cur);
    if (lid_clicked >= 0 && lid_clicked != lid_cur) {
        snprintf(lc.handle_lid_switch, sizeof(lc.handle_lid_switch), "%s", lid_vals[lid_clicked]);
        dc_logind_conf_write_dropin(&lc);
    }

    int lid_ac_cur = -1;
    for (int i = 0; i < 5; i++)
        if (strcmp(lc.handle_lid_switch_external_power, lid_vals[i]) == 0)
            lid_ac_cur = i;
    int lid_ac_clicked = ui_segmented(c, "Lid close on AC power", lid_opts, 5, lid_ac_cur);
    if (lid_ac_clicked >= 0 && lid_ac_clicked != lid_ac_cur) {
        snprintf(lc.handle_lid_switch_external_power, sizeof(lc.handle_lid_switch_external_power),
                 "%s", lid_vals[lid_ac_clicked]);
        dc_logind_conf_write_dropin(&lc);
    }

    ui_hint(c, "Per-power-source idle timeouts need a dedicated idle-detection service");
    ui_hint(c, "(not available yet) -- \"When idle\" above applies regardless of AC state.");
    ui_hint(c, "Writes /etc/systemd/logind.conf.d/50-dankc.conf via pkexec;");
    ui_hint(c, "a re-login (or reboot) is needed for changes to take effect.");
}

/* ====================== niri Window Rules editor (W3.4) ======================
 *
 * docs/14-COMPLETION-PLAN.md W3.4 ("niri window-rules editor"). Safety design
 * (see also the task's own "do NOT corrupt the user's niri config" mandate):
 *
 *   - dankc NEVER rewrites the user's hand-written config.kdl content. The
 *     only rules dankc can add/remove through this UI live in a fully
 *     dankc-owned file, ~/.config/niri/dankc-rules.kdl -- freely rewritten
 *     from what this tab currently understands, same trust level as
 *     config.json itself (we generated it, we can regenerate it).
 *   - The ONE place this code touches the user's real config.kdl is adding a
 *     single `include "dankc-rules.kdl"` line, and ONLY when the user clicks
 *     an explicit "Enable" row (never automatically) -- gated behind a
 *     mandatory timestamped backup written first (wr_ensure_include()).
 *   - Rules already present in the user's config.kdl (or anything *it*
 *     includes) are parsed and listed for visibility, but rendered
 *     read-only -- no remove/edit affordance, matching the task's fallback
 *     of "read + stage" for anything too risky to auto-apply.
 *   - Parsing is intentionally tolerant, not a full KDL implementation --
 *     same philosophy as src/ui/keybinds_modal.c's own doc comment, and this
 *     block reuses that file's brace/quote-aware block-scanning approach
 *     (kept as an independent copy here per this task's file-ownership
 *     boundary: this agent owns settings.c only). Recognizes exactly the
 *     grammar this editor itself writes (`match app-id="..."`,
 *     `open-floating <bool>`, `open-maximized <bool>`, `opacity <float>`);
 *     anything else in a window-rule block (geometry, default-column-width,
 *     draw-border-with-background, ...) is silently ignored for display
 *     purposes and left completely untouched on disk for read-only rules.
 */
#define DC_WR_MAX 32
#define DC_WR_APPID_MAX 160
#define DC_WR_MAX_INCLUDE_FILES 16
#define DC_WR_MAX_DEPTH 6

typedef struct {
    char app_id[DC_WR_APPID_MAX]; /* first `match app-id="..."` value found, if any */
    int extra_matches;            /* additional `match` lines beyond the first (OR'd) */
    bool has_open_floating;
    bool open_floating;
    bool has_open_maximized;
    bool open_maximized;
    bool has_opacity;
    float opacity;
    bool managed; /* true => parsed from dankc-rules.kdl (safe to remove/rewrite) */
} wr_rule;

static wr_rule g_wr_rules[DC_WR_MAX];
static int g_wr_n = 0;
static bool g_wr_dirty = true; /* reparse from disk next time the tab is shown */
static bool g_wr_managed_exists = false;
static bool g_wr_include_present = false; /* config.kdl already has `include "dankc-rules.kdl"` */
static char g_wr_managed_path[DC_CONFIG_PATH_MAX];
static char g_wr_config_path[DC_CONFIG_PATH_MAX];

static char *wr_read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }
    long sz = ftell(f);
    if (sz < 0 || sz > 4 * 1024 * 1024) { /* sanity cap, matches keybinds_modal.c's */
        fclose(f);
        return NULL;
    }
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t n = fread(buf, 1, (size_t)sz, f);
    fclose(f);
    buf[n] = '\0';
    return buf;
}

/* Blank out `// ...` line comments in place (same tolerant approach as
 * keybinds_modal.c's kb_strip_comments -- doesn't special-case `//` inside a
 * quoted string, which real niri configs never put there). */
static void wr_strip_comments(char *text)
{
    for (char *p = text; *p; p++) {
        if (p[0] == '/' && p[1] == '/') {
            while (*p && *p != '\n')
                *p++ = ' ';
            if (!*p)
                break;
        }
    }
}

typedef void (*wr_block_cb)(const char *inner, size_t len, void *ctx);

/* Find every `<tag> { ... }` node anywhere in `text` (word-boundary checked,
 * brace/quote aware so nested braces or braces-in-strings don't confuse the
 * depth count) and hand its inner content to `cb`. Generalized copy of
 * keybinds_modal.c's kb_scan_binds_blocks (parameterized by tag name; that
 * file scans for "binds" specifically, this one for "window-rule"). */
static void wr_scan_blocks(const char *text, const char *tag, wr_block_cb cb, void *ctx)
{
    size_t taglen = strlen(tag);
    const char *p = text;
    while ((p = strstr(p, tag)) != NULL) {
        bool left_ok =
            (p == text) || !(isalnum((unsigned char)p[-1]) || p[-1] == '_' || p[-1] == '-');
        const char *after = p + taglen;
        bool right_ok =
            !(isalnum((unsigned char)after[0]) || after[0] == '_' || after[0] == '-');
        if (left_ok && right_ok) {
            const char *q = after;
            while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r')
                q++;
            if (*q == '{') {
                const char *inner_start = q + 1;
                int depth = 1;
                const char *r = inner_start;
                while (*r && depth > 0) {
                    if (*r == '"') {
                        r++;
                        while (*r && *r != '"') {
                            if (*r == '\\' && r[1])
                                r++;
                            r++;
                        }
                        if (*r)
                            r++;
                        continue;
                    }
                    if (*r == '{')
                        depth++;
                    else if (*r == '}') {
                        depth--;
                        if (depth == 0)
                            break;
                    }
                    r++;
                }
                if (depth == 0) {
                    cb(inner_start, (size_t)(r - inner_start), ctx);
                    p = r + 1;
                    continue;
                }
            }
        }
        p = after;
    }
}

/* Word-boundary check for a keyword at `text[pos]`, bounded to [0, textlen). */
static bool wr_word_at(const char *text, size_t pos, size_t textlen, const char *word)
{
    size_t wl = strlen(word);
    if (pos + wl > textlen)
        return false;
    if (strncmp(text + pos, word, wl) != 0)
        return false;
    bool left_ok =
        (pos == 0) || !(isalnum((unsigned char)text[pos - 1]) || text[pos - 1] == '_' ||
                        text[pos - 1] == '-');
    unsigned char right = (pos + wl < textlen) ? (unsigned char)text[pos + wl] : 0;
    bool right_ok = !(isalnum(right) || right == '_' || right == '-');
    return left_ok && right_ok;
}

/* Find `needle` (e.g. "app-id=") within [start,end) and copy the quoted value
 * that follows it -- handles both plain `"foo"` and niri's raw-string
 * `r#"foo"#`/`r##"foo"##` forms transparently, since only the content between
 * the first `"` after `needle` and its matching closing `"` is copied (the
 * leading `r#`/trailing `#` markers sit outside the quotes and are never
 * touched). */
static void wr_extract_quoted_in_range(const char *text, size_t start, size_t end,
                                       const char *needle, char *out, size_t outsz)
{
    out[0] = '\0';
    size_t nlen = strlen(needle);
    size_t found = (size_t)-1;
    for (size_t i = start; i + nlen <= end; i++) {
        if (strncmp(text + i, needle, nlen) == 0) {
            found = i;
            break;
        }
    }
    if (found == (size_t)-1)
        return;
    size_t p = found + nlen;
    while (p < end && text[p] != '"') {
        if (text[p] == ';' || text[p] == '\n')
            return;
        p++;
    }
    if (p >= end || text[p] != '"')
        return;
    p++;
    size_t n = 0;
    while (p < end && text[p] != '"' && n + 1 < outsz) {
        if (text[p] == '\\' && p + 1 < end)
            p++;
        out[n++] = text[p++];
    }
    out[n] = '\0';
}

/* First "true"/"false" token in [start,end); bare/unrecognized defaults to
 * true (every hand-written example seen uses an explicit bool, but a bare
 * flag reads more naturally as "on" than "off" if that ever changes). */
static bool wr_bool_after(const char *text, size_t start, size_t end)
{
    for (size_t i = start; i < end; i++) {
        if (wr_word_at(text, i, end, "false"))
            return false;
        if (wr_word_at(text, i, end, "true"))
            return true;
    }
    return true;
}

static float wr_float_after(const char *text, size_t start, size_t end)
{
    size_t i = start;
    while (i < end && (text[i] == ' ' || text[i] == '\t'))
        i++;
    char buf[32];
    size_t n = 0;
    while (i < end && n + 1 < sizeof(buf) &&
           (isdigit((unsigned char)text[i]) || text[i] == '.' || text[i] == '-'))
        buf[n++] = text[i++];
    buf[n] = '\0';
    if (n == 0)
        return 1.0f;
    return (float)atof(buf);
}

/* Parse one `window-rule { ... }` block's inner content into a wr_rule.
 * Walks top-level statements (depth-0, newline- or `{`-delimited; a quoted
 * span is treated as opaque so a brace/newline inside a regex string can't
 * end a statement early; a nested `key { ... }` block, e.g.
 * default-column-width's, is swallowed whole via brace/quote-aware depth
 * tracking without being individually inspected -- this editor doesn't
 * understand or touch those properties). */
static void wr_parse_rule_inner(const char *inner, size_t len, bool managed)
{
    if (g_wr_n >= DC_WR_MAX)
        return;
    wr_rule r;
    memset(&r, 0, sizeof(r));
    r.managed = managed;
    int match_count = 0;

    size_t i = 0;
    while (i < len) {
        while (i < len && isspace((unsigned char)inner[i]))
            i++;
        if (i >= len)
            break;
        size_t line_start = i;

        size_t head_end = i;
        while (head_end < len && inner[head_end] != '\n' && inner[head_end] != '{') {
            if (inner[head_end] == '"') {
                head_end++;
                while (head_end < len && inner[head_end] != '"') {
                    if (inner[head_end] == '\\' && head_end + 1 < len)
                        head_end++;
                    head_end++;
                }
                if (head_end < len)
                    head_end++;
                continue;
            }
            head_end++;
        }

        size_t stmt_end = head_end;
        if (head_end < len && inner[head_end] == '{') {
            int depth = 1;
            size_t j = head_end + 1;
            while (j < len && depth > 0) {
                if (inner[j] == '"') {
                    j++;
                    while (j < len && inner[j] != '"') {
                        if (inner[j] == '\\' && j + 1 < len)
                            j++;
                        j++;
                    }
                    if (j < len)
                        j++;
                    continue;
                }
                if (inner[j] == '{')
                    depth++;
                else if (inner[j] == '}')
                    depth--;
                j++;
            }
            stmt_end = j;
        }

        if (wr_word_at(inner, line_start, head_end, "match")) {
            char appid[DC_WR_APPID_MAX];
            wr_extract_quoted_in_range(inner, line_start, head_end, "app-id=", appid,
                                       sizeof(appid));
            if (match_count == 0 && appid[0])
                copy_trunc(r.app_id, sizeof(r.app_id), appid);
            else if (match_count > 0)
                r.extra_matches++;
            match_count++;
        } else if (wr_word_at(inner, line_start, head_end, "open-floating")) {
            r.has_open_floating = true;
            r.open_floating = wr_bool_after(inner, line_start + strlen("open-floating"), head_end);
        } else if (wr_word_at(inner, line_start, head_end, "open-maximized")) {
            r.has_open_maximized = true;
            r.open_maximized =
                wr_bool_after(inner, line_start + strlen("open-maximized"), head_end);
        } else if (wr_word_at(inner, line_start, head_end, "opacity")) {
            r.has_opacity = true;
            r.opacity = wr_float_after(inner, line_start + strlen("opacity"), head_end);
        }

        i = stmt_end;
    }

    if (g_wr_n < DC_WR_MAX)
        g_wr_rules[g_wr_n++] = r;
}

static void wr_collect_cb(const char *inner, size_t len, void *ctx)
{
    bool managed = *(const bool *)ctx;
    wr_parse_rule_inner(inner, len, managed);
}

/* Read one KDL file, scan it for window-rule blocks (tagged managed iff the
 * file is exactly ~/.config/niri/dankc-rules.kdl), then follow its
 * `include "..."` statements (same tolerant, depth+seen-guarded recursion as
 * keybinds_modal.c's kb_parse_file, kept as an independent copy here). */
static void wr_parse_file(const char *path, int depth, char seen[][DC_CONFIG_PATH_MAX],
                          int *seen_n)
{
    if (depth > DC_WR_MAX_DEPTH)
        return;
    for (int i = 0; i < *seen_n; i++)
        if (strcmp(seen[i], path) == 0)
            return;
    if (*seen_n < DC_WR_MAX_INCLUDE_FILES)
        snprintf(seen[(*seen_n)++], DC_CONFIG_PATH_MAX, "%s", path);

    char *text = wr_read_file(path);
    if (!text)
        return;
    wr_strip_comments(text);

    bool managed = strcmp(path, g_wr_managed_path) == 0;
    wr_scan_blocks(text, "window-rule", wr_collect_cb, &managed);

    char dir[DC_CONFIG_PATH_MAX];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash)
        *slash = '\0';
    else
        snprintf(dir, sizeof(dir), ".");

    const char *p = text;
    while ((p = strstr(p, "include")) != NULL) {
        const char *q = p + 7;
        while (*q == ' ' || *q == '\t')
            q++;
        if (*q == '"') {
            q++;
            const char *end = strchr(q, '"');
            if (end && end > q) {
                char rel[300];
                size_t rl = (size_t)(end - q);
                if (rl >= sizeof(rel))
                    rl = sizeof(rel) - 1;
                memcpy(rel, q, rl);
                rel[rl] = '\0';
                char childpath[sizeof(dir) + sizeof(rel) + 2];
                snprintf(childpath, sizeof(childpath), "%s/%s", dir, rel);
                wr_parse_file(childpath, depth + 1, seen, seen_n);
            }
            p = end ? end + 1 : q;
        } else {
            p = q;
        }
    }
    free(text);
}

/* Entry point: resolve paths, check whether config.kdl already includes our
 * managed file, then parse config.kdl (recursively) plus -- if not already
 * reached via an include -- dankc-rules.kdl directly, so staged rules are
 * still visible/manageable before the user clicks "Enable". */
static void wr_load(void)
{
    g_wr_n = 0;
    g_wr_include_present = false;
    g_wr_managed_exists = false;

    const char *home = getenv("HOME");
    if (!home || !home[0]) {
        dc_warn("window rules: $HOME unset, cannot locate config.kdl");
        return;
    }
    snprintf(g_wr_config_path, sizeof(g_wr_config_path), "%s/.config/niri/config.kdl", home);
    snprintf(g_wr_managed_path, sizeof(g_wr_managed_path), "%s/.config/niri/dankc-rules.kdl",
            home);

    struct stat st;
    g_wr_managed_exists = stat(g_wr_managed_path, &st) == 0;

    char *cfg_text = wr_read_file(g_wr_config_path);
    if (cfg_text) {
        g_wr_include_present = strstr(cfg_text, "include \"dankc-rules.kdl\"") != NULL;
        free(cfg_text);
    }

    char seen[DC_WR_MAX_INCLUDE_FILES][DC_CONFIG_PATH_MAX];
    int seen_n = 0;
    wr_parse_file(g_wr_config_path, 0, seen, &seen_n);

    bool managed_seen = false;
    for (int i = 0; i < seen_n; i++)
        if (strcmp(seen[i], g_wr_managed_path) == 0)
            managed_seen = true;
    if (!managed_seen)
        wr_parse_file(g_wr_managed_path, 0, seen, &seen_n);

    dc_info("window rules: parsed %d rule(s) from %d file(s) (managed file %s, included=%d)",
            g_wr_n, seen_n, g_wr_managed_exists ? "exists" : "absent", g_wr_include_present);
}

static void wr_ensure_loaded(void)
{
    if (g_wr_dirty) {
        wr_load();
        g_wr_dirty = false;
    }
}

static void wr_serialize_rule(FILE *f, const wr_rule *r)
{
    fprintf(f, "window-rule {\n");
    if (r->app_id[0])
        fprintf(f, "    match app-id=\"%s\"\n", r->app_id);
    if (r->has_open_floating)
        fprintf(f, "    open-floating %s\n", r->open_floating ? "true" : "false");
    if (r->has_open_maximized)
        fprintf(f, "    open-maximized %s\n", r->open_maximized ? "true" : "false");
    if (r->has_opacity)
        fprintf(f, "    opacity %.2f\n", (double)r->opacity);
    fprintf(f, "}\n");
}

/* Rewrite ~/.config/niri/dankc-rules.kdl from the in-memory MANAGED subset of
 * g_wr_rules. Fully dankc-owned (see the block comment above) -- no backup
 * needed, unlike wr_ensure_include() below. Anything in a prior hand-edited
 * version of this file that this parser doesn't recognize is lost on the
 * next add/remove through the UI; the header comment warns about this. */
static bool wr_write_managed_file(void)
{
    if (!g_wr_managed_path[0])
        return false;
    FILE *f = fopen(g_wr_managed_path, "w");
    if (!f) {
        dc_warn("window rules: could not write %s", g_wr_managed_path);
        return false;
    }
    fputs("// Managed by DankC's Settings > Window Rules tab.\n"
         "// Hand edits are fine (dankc's tolerant parser will pick them up), but\n"
         "// adding/removing a rule through the UI rewrites this whole file from\n"
         "// what dankc currently understands -- anything it can't parse (KDL\n"
         "// features beyond match app-id / open-floating / open-maximized /\n"
         "// opacity) will be lost on the next UI edit.\n\n",
         f);
    for (int i = 0; i < g_wr_n; i++)
        if (g_wr_rules[i].managed)
            wr_serialize_rule(f, &g_wr_rules[i]);
    fclose(f);
    g_wr_managed_exists = true;
    return true;
}

/* The ONE write to the user's real niri config.kdl this editor ever makes:
 * appending a single `include "dankc-rules.kdl"` line, backed up first. Never
 * called automatically -- only from an explicit "Enable" row click. */
static bool wr_ensure_include(void)
{
    if (!g_wr_config_path[0])
        wr_load();
    if (!g_wr_config_path[0])
        return false;

    char *text = wr_read_file(g_wr_config_path);
    if (!text) {
        dc_warn("window rules: could not read %s to add the include", g_wr_config_path);
        return false;
    }
    if (strstr(text, "include \"dankc-rules.kdl\"")) {
        free(text);
        g_wr_include_present = true;
        return true; /* already there (e.g. added by hand) */
    }

    char backup_path[DC_CONFIG_PATH_MAX + 32];
    snprintf(backup_path, sizeof(backup_path), "%s.bak-%ld", g_wr_config_path, (long)time(NULL));
    FILE *bf = fopen(backup_path, "w");
    if (!bf) {
        dc_warn("window rules: could not create backup %s; aborting include", backup_path);
        free(text);
        return false;
    }
    fputs(text, bf);
    fclose(bf);
    free(text);

    FILE *f = fopen(g_wr_config_path, "a");
    if (!f) {
        dc_warn("window rules: could not append to %s (backup at %s is safe to restore)",
                g_wr_config_path, backup_path);
        return false;
    }
    fprintf(f, "\n// Added by DankC Settings > Window Rules (backup: %s):\n", backup_path);
    fputs("include \"dankc-rules.kdl\"\n", f);
    fclose(f);
    dc_info("window rules: added include to %s (backup %s)", g_wr_config_path, backup_path);
    g_wr_include_present = true;
    return true;
}

static void wr_add_rule(dc_settings *s)
{
    if (!s->wr_new_app_id[0] && !s->wr_new_floating && !s->wr_new_maximized &&
        !s->wr_new_opacity_enabled)
        return; /* nothing to add */
    if (g_wr_n >= DC_WR_MAX) {
        dc_warn("window rules: at the %d-rule display cap, not adding another", DC_WR_MAX);
        return;
    }
    if (!g_wr_managed_path[0])
        wr_load();

    wr_rule r;
    memset(&r, 0, sizeof(r));
    r.managed = true;
    copy_trunc(r.app_id, sizeof(r.app_id), s->wr_new_app_id);
    if (s->wr_new_floating) {
        r.has_open_floating = true;
        r.open_floating = true;
    }
    if (s->wr_new_maximized) {
        r.has_open_maximized = true;
        r.open_maximized = true;
    }
    if (s->wr_new_opacity_enabled) {
        r.has_opacity = true;
        r.opacity = s->wr_new_opacity;
    }

    g_wr_rules[g_wr_n++] = r;
    if (wr_write_managed_file()) {
        s->wr_new_app_id[0] = '\0';
        s->wr_new_floating = false;
        s->wr_new_maximized = false;
        s->wr_new_opacity_enabled = false;
        s->wr_new_opacity = 1.0f;
    }
    g_wr_dirty = true; /* reparse so the list reflects our own canonical formatting */
}

static void wr_remove_rule(int idx)
{
    if (idx < 0 || idx >= g_wr_n || !g_wr_rules[idx].managed)
        return;
    for (int i = idx; i < g_wr_n - 1; i++)
        g_wr_rules[i] = g_wr_rules[i + 1];
    g_wr_n--;
    wr_write_managed_file();
    g_wr_dirty = true;
}

static void wr_summarize_actions(const wr_rule *r, char *out, size_t outsz)
{
    char parts[4][40];
    int n = 0;
    if (r->has_open_floating && r->open_floating && n < 4)
        snprintf(parts[n++], sizeof(parts[0]), "float");
    if (r->has_open_maximized && r->open_maximized && n < 4)
        snprintf(parts[n++], sizeof(parts[0]), "maximized");
    if (r->has_opacity && n < 4)
        snprintf(parts[n++], sizeof(parts[0]), "opacity %.0f%%", (double)r->opacity * 100.0);
    if (r->extra_matches > 0 && n < 4)
        snprintf(parts[n++], sizeof(parts[0]), "+%d more match", r->extra_matches);
    out[0] = '\0';
    for (int i = 0; i < n; i++) {
        if (i > 0)
            snprintf(out + strlen(out), outsz - strlen(out), " \xc2\xb7 ");
        snprintf(out + strlen(out), outsz - strlen(out), "%s", parts[i]);
    }
    if (n == 0)
        snprintf(out, outsz, "no recognized actions");
}

/* docs/14-COMPLETION-PLAN.md W3.4: list existing rules (dankc-managed ones
 * removable; everything else from the user's own config.kdl read-only) and a
 * simple add-a-rule form (app-id match -> float/maximize/opacity), matching
 * the task's own scoped-down subset of DMS's WindowRulesTab.qml/
 * WindowRuleModal.qml. See the safety block comment above wr_scan_blocks(). */
static void tab_window_rules(uictx *c)
{
    wr_ensure_loaded();

    ui_section(c, "STATUS");
    int managed_n = 0, unmanaged_n = 0;
    for (int i = 0; i < g_wr_n; i++) {
        if (g_wr_rules[i].managed)
            managed_n++;
        else
            unmanaged_n++;
    }
    char status[96];
    snprintf(status, sizeof(status), "%d dankc-managed, %d read-only", managed_n, unmanaged_n);
    ui_value(c, "Rules found", status);
    ui_value(c, "~/.config/niri/dankc-rules.kdl",
             g_wr_managed_exists ? "exists" : "not created yet");

    if (!g_wr_include_present) {
        ui_hint(c, "niri isn't including dankc-rules.kdl yet -- rules added below");
        ui_hint(c, "won't take effect until you enable it (backs up config.kdl first).");
        if (ui_list_row(c, "Enable dankc-rules.kdl in niri config", NULL, IC_DONE, false) == 1) {
            wr_ensure_include();
        }
    } else {
        ui_hint(c, "config.kdl already includes dankc-rules.kdl -- niri picks up");
        ui_hint(c, "new rules on its own (it watches the file for changes).");
    }

    if (managed_n > 0) {
        ui_section(c, "YOUR RULES (dankc-managed)");
        for (int i = 0; i < g_wr_n; i++) {
            if (!g_wr_rules[i].managed)
                continue;
            char summary[128];
            wr_summarize_actions(&g_wr_rules[i], summary, sizeof(summary));
            const char *title = g_wr_rules[i].app_id[0] ? g_wr_rules[i].app_id : "(any window)";
            if (ui_list_row(c, title, summary, IC_REMOVE, false) == 2) {
                wr_remove_rule(i);
                break; /* array shifted -- stop iterating this pass */
            }
        }
    }

    if (unmanaged_n > 0) {
        ui_section(c, "EXISTING RULES (read-only, from your niri config)");
        for (int i = 0; i < g_wr_n; i++) {
            if (g_wr_rules[i].managed)
                continue;
            char summary[128];
            wr_summarize_actions(&g_wr_rules[i], summary, sizeof(summary));
            const char *title = g_wr_rules[i].app_id[0] ? g_wr_rules[i].app_id : "(any window)";
            ui_list_row(c, title, summary, 0, false);
        }
    }
    if (g_wr_n == 0)
        ui_hint(c, "No window-rule blocks found in ~/.config/niri/config.kdl");

    ui_section(c, "ADD RULE");
    bool appid_focus = c->s->focus_field == 6;
    char appidbuf[DC_WR_APPID_MAX];
    if (appid_focus)
        copy_trunc(appidbuf, sizeof(appidbuf), c->s->edit_buf);
    else
        copy_trunc(appidbuf, sizeof(appidbuf), c->s->wr_new_app_id);
    if (ui_textfield(c, "App ID (e.g. \"firefox\" -- see `niri msg windows`)", appidbuf,
                     appid_focus)) {
        c->s->focus_field = 6;
        copy_trunc(c->s->edit_buf, sizeof(c->s->edit_buf), c->s->wr_new_app_id);
    }
    if (ui_toggle(c, "Open floating", NULL, c->s->wr_new_floating))
        c->s->wr_new_floating = !c->s->wr_new_floating;
    if (ui_toggle(c, "Open maximized", NULL, c->s->wr_new_maximized))
        c->s->wr_new_maximized = !c->s->wr_new_maximized;
    if (ui_toggle(c, "Set opacity", NULL, c->s->wr_new_opacity_enabled))
        c->s->wr_new_opacity_enabled = !c->s->wr_new_opacity_enabled;
    if (c->s->wr_new_opacity_enabled) {
        char ov[16];
        snprintf(ov, sizeof(ov), "%.0f%%", (double)c->s->wr_new_opacity * 100.0);
        ui_slider(c, "Opacity", &c->s->wr_new_opacity, 0.1f, 1.0f, ov);
    }
    bool can_add = c->s->wr_new_app_id[0] || c->s->wr_new_floating || c->s->wr_new_maximized ||
                  c->s->wr_new_opacity_enabled;
    if (ui_list_row(c,
                    can_add ? "Add rule to dankc-rules.kdl"
                            : "Add rule (fill in something above first)",
                    NULL, IC_ADD, false) == 1 &&
        can_add) {
        wr_add_rule(c->s);
    }
}

/* ====================== Keybinds tab (docs/23-KEYBIND-EDITING-PLAN.md,
 * KB-T3) ======================
 *
 * Cloned from tab_window_rules()/wr_* above (managed-vs-read-only CRUD
 * shape: STATUS -> YOUR BINDS (managed, removable) -> EXISTING BINDS
 * (read-only) -> RESET -> ADD form), but unlike wr_* -- which is its own
 * hand-rolled KDL parser/writer because it predates services/keybinds.h --
 * this tab is a thin view over the KB-T1 service: dc_keybinds_load()/
 * dc_keybinds_persist() own all the parsing, serialization, include-wiring,
 * backup and rollback; kb_* here only tracks which of the loaded binds are
 * "ours" and builds the in-progress add-form draft (dc_settings.kb_*
 * fields).
 */

#define DC_KB_TAB_MAX 512

static dc_keybind g_kb_all[DC_KB_TAB_MAX];
static int g_kb_n = 0;
static bool g_kb_dirty = true; /* reload from disk (via the service) next render */

static void kb_ensure_loaded(void)
{
    if (!g_kb_dirty)
        return;
    g_kb_n = dc_keybinds_load(g_kb_all, DC_KB_TAB_MAX, NULL);
    g_kb_dirty = false;
}

/* Rebuilds the managed subset of g_kb_all[] (optionally skipping
 * `drop_idx`, for a "replace" -- pass -1 for a plain resync) and hands it to
 * dc_keybinds_persist(), which owns the actual
 * ~/.config/niri/dankc-binds.kdl rewrite + include-wiring + validate/
 * rollback (all gated by $DANKC_BINDS_DRYRUN, same as every other write
 * this service performs). Always marks the cache dirty afterward so the
 * next render reflects whatever actually landed on disk (including a
 * rolled-back fragment, in the DC_KEYBINDS_VALIDATE_FAILED_ROLLED_BACK
 * case). */
static void kb_persist_managed(int drop_idx)
{
    dc_keybind managed[DC_KB_TAB_MAX];
    int mn = 0;
    for (int i = 0; i < g_kb_n && mn < DC_KB_TAB_MAX; i++) {
        if (!g_kb_all[i].managed || i == drop_idx)
            continue;
        managed[mn++] = g_kb_all[i];
    }
    dc_keybinds_persist(managed, mn, NULL);
    g_kb_dirty = true;
}

static void kb_remove(int idx)
{
    if (idx < 0 || idx >= g_kb_n || !g_kb_all[idx].managed)
        return;
    kb_persist_managed(idx);
}

/* RESET: an empty managed fragment. Never touches config.kdl or any
 * unmanaged bind -- dc_keybinds_persist() only ever rewrites dankc-binds.kdl
 * itself (see keybinds.h's file header). */
static void kb_reset(void)
{
    dc_keybinds_persist(NULL, 0, NULL);
    g_kb_dirty = true;
}

static void kb_capture_end(dc_settings *s)
{
    s->kb_capture = false;
    dc_wayland_shortcuts_uninhibit(s->wl);
}

static bool kb_is_modifier_keysym(uint32_t sym)
{
    switch (sym) {
    case XKB_KEY_Shift_L:
    case XKB_KEY_Shift_R:
    case XKB_KEY_Control_L:
    case XKB_KEY_Control_R:
    case XKB_KEY_Alt_L:
    case XKB_KEY_Alt_R:
    case XKB_KEY_Super_L:
    case XKB_KEY_Super_R:
    case XKB_KEY_Meta_L:
    case XKB_KEY_Meta_R:
    case XKB_KEY_Hyper_L:
    case XKB_KEY_Hyper_R:
    case XKB_KEY_Caps_Lock:
    case XKB_KEY_Num_Lock:
    case XKB_KEY_ISO_Level3_Shift:
    case XKB_KEY_ISO_Level5_Shift:
        return true;
    default:
        return false;
    }
}

/* dc_settings_handle_key()'s capture branch (below): Esc cancels, a bare
 * modifier keysym is ignored (just triggers a redraw so the "Mod+Ctrl+..."
 * live display tracks the held modifiers), anything else ends capture and,
 * if dc_keybinds_chord_from_capture() can resolve it (it refuses a base key
 * that is itself a modifier), stores the chord into kb_new_chord. */
static void kb_capture_key(dc_settings *s, uint32_t keysym)
{
    if (keysym == XKB_KEY_Escape) {
        kb_capture_end(s);
        s_render(s);
        return;
    }
    if (kb_is_modifier_keysym(keysym)) {
        s_render(s);
        return;
    }
    uint32_t base = dc_wayland_base_keysym(s->wl);
    bool super = dc_wayland_super_down(s->wl);
    bool ctrl = dc_wayland_ctrl_down(s->wl);
    bool alt = dc_wayland_alt_down(s->wl);
    bool shift = dc_wayland_shift_down(s->wl);
    char chord[64];
    if (dc_keybinds_chord_from_capture(base, super, ctrl, alt, shift, chord, sizeof(chord)))
        copy_trunc(s->kb_new_chord, sizeof(s->kb_new_chord), chord);
    kb_capture_end(s);
    s_render(s);
}

/* Builds `prefix + (escaped, if requested) body + suffix`, bounded-truncating
 * into `out`. Deliberately hand-copied (memcpy + manual index bounds) rather
 * than snprintf(..., "%s", body): body's declared size (kb_custom_cmd/preset
 * verb) can exceed out's DC_KEYBIND_ACTION_MAX budget once the literal
 * prefix/suffix are accounted for, which is exactly the
 * -Wformat-truncation shape copy_trunc()'s own comment (top of this file)
 * warns about -- same fix, applied here for a wrapped-not-identity copy. */
static void kb_build_spawn_action(const char *prefix, const char *body, bool escape_body,
                                  const char *suffix, char *out, size_t outsz)
{
    size_t oi = 0;
    size_t plen = strlen(prefix);
    if (plen >= outsz)
        plen = outsz > 0 ? outsz - 1 : 0;
    memcpy(out, prefix, plen);
    oi = plen;
    size_t slen = strlen(suffix);
    for (const char *p = body; *p && oi + slen + 1 < outsz; p++) {
        if (escape_body && (*p == '"' || *p == '\\')) {
            if (oi + slen + 1 >= outsz)
                break;
            out[oi++] = '\\';
        }
        if (oi + slen + 1 < outsz)
            out[oi++] = *p;
    }
    if (oi + slen < outsz) {
        memcpy(out + oi, suffix, slen);
        oi += slen;
    }
    out[oi] = '\0';
}

/* Resets the add-form draft after a successful add (or a Replace). */
static void kb_add_form_reset(dc_settings *s)
{
    s->kb_new_chord[0] = '\0';
    s->kb_mode = 0;
    s->kb_niri_idx = -1;
    s->kb_dankc_idx = -1;
    s->kb_custom_cmd[0] = '\0';
    s->kb_title[0] = '\0';
}

/* Builds the dc_keybind from the current add-form draft and persists it,
 * replacing `replace_idx`'s managed bind if >= 0 (the "Replace conflicting
 * bind" path -- see tab_keybinds()'s conflict line), else appending. */
static void kb_add(dc_settings *s, int replace_idx)
{
    if (!s->kb_new_chord[0])
        return;

    dc_keybind nb;
    memset(&nb, 0, sizeof(nb));
    copy_trunc(nb.chord, sizeof(nb.chord), s->kb_new_chord);
    copy_trunc(nb.title, sizeof(nb.title), s->kb_title);
    copy_trunc(nb.source, sizeof(nb.source), "dankc-binds.kdl");
    nb.managed = true;

    if (s->kb_mode == 0) {
        int count = 0;
        const dc_keybind_action_preset *p = dc_keybinds_niri_actions(&count);
        if (s->kb_niri_idx < 0 || s->kb_niri_idx >= count)
            return;
        copy_trunc(nb.action, sizeof(nb.action), p[s->kb_niri_idx].verb);
    } else if (s->kb_mode == 1) {
        int count = 0;
        const dc_keybind_action_preset *p = dc_keybinds_dankc_actions(&count);
        if (s->kb_dankc_idx < 0 || s->kb_dankc_idx >= count)
            return;
        kb_build_spawn_action("spawn \"dankc\" \"ctl\" \"", p[s->kb_dankc_idx].verb, false, "\"",
                              nb.action, sizeof(nb.action));
    } else {
        if (!s->kb_custom_cmd[0])
            return;
        kb_build_spawn_action("spawn \"sh\" \"-c\" \"", s->kb_custom_cmd, true, "\"", nb.action,
                              sizeof(nb.action));
    }
    if (!nb.action[0])
        return;

    dc_keybind managed[DC_KB_TAB_MAX];
    int mn = 0;
    for (int i = 0; i < g_kb_n && mn < DC_KB_TAB_MAX; i++) {
        if (!g_kb_all[i].managed || i == replace_idx)
            continue;
        managed[mn++] = g_kb_all[i];
    }
    if (mn < DC_KB_TAB_MAX)
        managed[mn++] = nb;

    dc_keybinds_persist(managed, mn, NULL);
    g_kb_dirty = true;
    kb_add_form_reset(s);
}

static void tab_keybinds(uictx *c)
{
    dc_settings *s = c->s;
    kb_ensure_loaded();

    ui_section(c, "STATUS");
    int managed_n = 0, unmanaged_n = 0;
    for (int i = 0; i < g_kb_n; i++) {
        if (g_kb_all[i].managed)
            managed_n++;
        else
            unmanaged_n++;
    }
    char status[96];
    snprintf(status, sizeof(status), "%d total, %d dankc-managed, %d read-only", g_kb_n, managed_n,
             unmanaged_n);
    ui_value(c, "Binds found", status);

    switch (dc_keybinds_last_validate()) {
    case DC_KEYBINDS_VALIDATE_OK:
        ui_value(c, "Last niri validate", "OK");
        break;
    case DC_KEYBINDS_VALIDATE_FAILED:
        ui_hint(c, "Last save was rejected by `niri validate` (fragment left as written --");
        ui_hint(c, "see the log for detail; the write/rollback I/O itself failed).");
        break;
    case DC_KEYBINDS_VALIDATE_FAILED_ROLLED_BACK:
        ui_hint(c, "\xe2\x9a\xa0 change rejected by niri validate -- previous binds restored");
        break;
    case DC_KEYBINDS_VALIDATE_UNKNOWN:
    default:
        break; /* nothing saved yet this session, or niri/validate unavailable */
    }

    if (managed_n > 0) {
        ui_section(c, "YOUR BINDS");
        for (int i = 0; i < g_kb_n; i++) {
            if (!g_kb_all[i].managed)
                continue;
            const char *label = g_kb_all[i].title[0] ? g_kb_all[i].title : g_kb_all[i].action;
            if (ui_list_row(c, g_kb_all[i].chord, label, IC_REMOVE, false) == 2) {
                kb_remove(i);
                break; /* array reloaded from disk -- stop iterating this pass */
            }
        }
    }

    if (unmanaged_n > 0) {
        ui_section(c, "EXISTING BINDS (read-only, from your niri config)");
        for (int i = 0; i < g_kb_n; i++) {
            if (g_kb_all[i].managed)
                continue;
            char summary[DC_KEYBIND_ACTION_MAX + DC_KEYBIND_SOURCE_MAX];
            snprintf(summary, sizeof(summary), "%s \xc2\xb7 %s", g_kb_all[i].action,
                     g_kb_all[i].source);
            ui_list_row(c, g_kb_all[i].chord, summary, 0, false);
        }
    }
    if (g_kb_n == 0)
        ui_hint(c, "No keybinds found in ~/.config/niri/config.kdl");

    ui_section(c, "RESET");
    if (managed_n > 0) {
        if (ui_list_row(c, "Remove all dankc-managed binds", NULL, IC_REMOVE, false) == 1)
            kb_reset();
    } else {
        ui_hint(c, "No dankc-managed binds to reset");
    }

    ui_section(c, "ADD BIND");
    if (!s->kb_capture) {
        const char *row_title = s->kb_new_chord[0] ? s->kb_new_chord : "Record shortcut";
        const char *row_status = s->kb_new_chord[0] ? "click to re-record" : NULL;
        if (ui_list_row(c, row_title, row_status, IC_KEYBOARD, s->kb_new_chord[0] != 0) == 1) {
            s->kb_new_chord[0] = '\0';
            s->kb_capture = true;
            dc_wayland_shortcuts_inhibit(s->wl, s->surface);
        }
    } else {
        char live[64];
        snprintf(live, sizeof(live), "%s%s%s%s\xe2\x80\xa6", dc_wayland_super_down(s->wl) ? "Mod+" : "",
                 dc_wayland_ctrl_down(s->wl) ? "Ctrl+" : "", dc_wayland_alt_down(s->wl) ? "Alt+" : "",
                 dc_wayland_shift_down(s->wl) ? "Shift+" : "");
        ui_value(c, "Press the shortcut now\xe2\x80\xa6 (Esc cancels)", live);
    }

    static const char *const kb_mode_opts[3] = {"niri action", "dankc action", "custom command"};
    int seg = ui_segmented(c, "Action type", kb_mode_opts, 3, s->kb_mode);
    if (seg >= 0 && seg != s->kb_mode) {
        s->kb_mode = seg;
        s->kb_niri_idx = -1;
        s->kb_dankc_idx = -1;
    }

    if (s->kb_mode == 0) {
        int count = 0;
        const dc_keybind_action_preset *presets = dc_keybinds_niri_actions(&count);
        for (int i = 0; i < count; i++) {
            if (ui_list_row(c, presets[i].label, presets[i].cat, 0, s->kb_niri_idx == i) == 1)
                s->kb_niri_idx = i;
        }
    } else if (s->kb_mode == 1) {
        int count = 0;
        const dc_keybind_action_preset *presets = dc_keybinds_dankc_actions(&count);
        for (int i = 0; i < count; i++) {
            if (ui_list_row(c, presets[i].label, presets[i].cat, 0, s->kb_dankc_idx == i) == 1)
                s->kb_dankc_idx = i;
        }
    } else {
        bool cmd_focus = s->focus_field == 13;
        char cmdbuf[160];
        if (cmd_focus)
            copy_trunc(cmdbuf, sizeof(cmdbuf), s->edit_buf);
        else
            copy_trunc(cmdbuf, sizeof(cmdbuf), s->kb_custom_cmd);
        if (ui_textfield(c, "Custom command (spawned via sh -c)", cmdbuf, cmd_focus)) {
            s->focus_field = 13;
            copy_trunc(s->edit_buf, sizeof(s->edit_buf), s->kb_custom_cmd);
        }
    }

    bool title_focus = s->focus_field == 14;
    char titlebuf[DC_KEYBIND_TITLE_MAX];
    if (title_focus)
        copy_trunc(titlebuf, sizeof(titlebuf), s->edit_buf);
    else
        copy_trunc(titlebuf, sizeof(titlebuf), s->kb_title);
    if (ui_textfield(c, "Description (hotkey-overlay title, optional)", titlebuf, title_focus)) {
        s->focus_field = 14;
        copy_trunc(s->edit_buf, sizeof(s->edit_buf), s->kb_title);
    }

    int conflict_idx = -1;
    if (s->kb_new_chord[0])
        conflict_idx = dc_keybinds_find_conflict(g_kb_all, g_kb_n, s->kb_new_chord, -1);
    bool conflict_managed = conflict_idx >= 0 && g_kb_all[conflict_idx].managed;
    if (conflict_idx >= 0) {
        char cmsg[128];
        snprintf(cmsg, sizeof(cmsg), "Conflicts with %s bind on %s", g_kb_all[conflict_idx].chord,
                 conflict_managed ? "this same chord (yours)" : "a read-only chord");
        ui_hint(c, cmsg);
    }

    bool action_chosen = (s->kb_mode == 0 && s->kb_niri_idx >= 0) ||
                         (s->kb_mode == 1 && s->kb_dankc_idx >= 0) ||
                         (s->kb_mode == 2 && s->kb_custom_cmd[0] != '\0');
    bool can_add = s->kb_new_chord[0] && action_chosen && (conflict_idx < 0 || conflict_managed);
    const char *add_label;
    if (!s->kb_new_chord[0])
        add_label = "Add bind (record a shortcut first)";
    else if (!action_chosen)
        add_label = "Add bind (choose an action first)";
    else if (conflict_idx >= 0 && !conflict_managed)
        add_label = "Add bind (conflicts with a read-only bind)";
    else if (conflict_managed)
        add_label = "Replace conflicting bind";
    else
        add_label = "Add bind";

    if (ui_list_row(c, add_label, NULL, IC_ADD, false) == 1 && can_add)
        kb_add(s, conflict_managed ? conflict_idx : -1);
}

/* docs/14-COMPLETION-PLAN.md W3.2: a pragmatic subset of DMS's 23-setting
 * LockScreenTab.qml -- only the options ui/lock.c can actually honor (no
 * fingerprint/U2F/video-screensaver/per-monitor backend exists in dankc's
 * lock screen). All four keys live-apply the next time the lock screen is
 * shown (`dankc ctl lock`); none of them can be previewed while the lock
 * isn't active, since ext-session-lock intentionally has no "peek" mode. */
static void tab_lockscreen(uictx *c)
{
    ui_section(c, "CLOCK");
    if (ui_toggle(c, "Show clock", "Big HH:MM clock on the lock screen", c->cfg->lock_show_clock)) {
        c->cfg->lock_show_clock = !c->cfg->lock_show_clock;
        c->changed = true;
    }
    if (ui_toggle(c, "Show date", "Weekday and date under the clock", c->cfg->lock_show_date)) {
        c->cfg->lock_show_date = !c->cfg->lock_show_date;
        c->changed = true;
    }

    ui_section(c, "PASSWORD FIELD");
    if (ui_toggle(c, "Always show password field",
                  "If off, the field appears once you start typing", c->cfg->lock_show_password_field)) {
        c->cfg->lock_show_password_field = !c->cfg->lock_show_password_field;
        c->changed = true;
    }

    ui_section(c, "BACKGROUND");
    if (ui_toggle(c, "Use wallpaper background",
                  "Blurred wallpaper instead of a flat color (needs Material",
                  c->cfg->lock_use_wallpaper_bg)) {
        c->cfg->lock_use_wallpaper_bg = !c->cfg->lock_use_wallpaper_bg;
        c->changed = true;
    }
    ui_hint(c, "backgrounds + a wallpaper set on the Personalization tab)");

    ui_hint(c, "Try it: run \"dankc ctl lock\" (Esc/wrong password stays locked;");
    ui_hint(c, "unlock with your normal password).");
}

/* ====================== W5 stretch tabs (docs/14-COMPLETION-PLAN.md) ======================
 *
 * Lower-priority MISSING tabs, each read-only-safe: list real system state via
 * a real command, never mutate anything the user didn't explicitly ask for
 * (System Updater's "Check" button runs the read-only `checkupdates` query,
 * never an actual upgrade -- matches the task's explicit "do NOT run updates
 * automatically"). Multiplexer's "attach" action is the one exception that
 * launches something (a terminal), which is the whole point of the feature.
 */

#define DC_MUX_MAX 8
#define DC_MUX_NAME_MAX 64

typedef struct {
    char name[DC_MUX_NAME_MAX];
    int windows; /* tmux only; -1 if unknown (zellij doesn't report this via list-sessions) */
} mux_session;

/* `tmux list-sessions -F '#{session_name}:#{session_windows}'` -> one
 * "name:N" per line. */
static int mux_tmux_list(mux_session *out, int max)
{
    int n = 0;
    FILE *pipe = popen("tmux list-sessions -F '#{session_name}:#{session_windows}' 2>/dev/null", "r");
    if (!pipe)
        return 0;
    char line[160];
    while (n < max && fgets(line, sizeof(line), pipe)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = '\0';
        char *colon = strrchr(line, ':');
        if (!colon)
            continue;
        *colon = '\0';
        out[n].windows = atoi(colon + 1);
        copy_trunc(out[n].name, sizeof(out[n].name), line);
        n++;
    }
    pclose(pipe);
    return n;
}

/* `zellij list-sessions` output is one session per line, name first
 * (optionally followed by " [Created ...]"/"[current]" -- best-effort: take
 * the first whitespace-delimited token). Zellij isn't installed on the dev
 * machine this was written on, so this is unverified against real output;
 * kept intentionally tolerant (worst case a session name mis-parses and is
 * skipped, never a crash). */
static int mux_zellij_list(mux_session *out, int max)
{
    int n = 0;
    FILE *pipe = popen("zellij list-sessions 2>/dev/null", "r");
    if (!pipe)
        return 0;
    char line[160];
    while (n < max && fgets(line, sizeof(line), pipe)) {
        size_t len = strcspn(line, " \t\n\r");
        if (len == 0 || len >= sizeof(out[0].name))
            continue;
        memcpy(out[n].name, line, len);
        out[n].name[len] = '\0';
        out[n].windows = -1;
        n++;
    }
    pclose(pipe);
    return n;
}

#define DC_MUX_CMD_MAX 200

/* Try a handful of common terminal emulators in order and run `inner_cmd`
 * inside whichever is found first, detached (fire-and-forget, matching
 * run_detached()'s existing DANKC_SETTINGS_DRYRUN gate below). `inner_cmd` is
 * a fixed-size array (not just `const char *`) so its bound stays visible to
 * the caller's -Wformat-truncation analysis all the way through -- see
 * copy_trunc()'s doc comment at the top of this file for the same rationale. */
static void mux_launch_terminal(const char inner_cmd[DC_MUX_CMD_MAX])
{
    char cmd[2 * DC_MUX_CMD_MAX + 200];
    snprintf(cmd, sizeof(cmd),
             "for t in foot kitty alacritty wezterm ghostty xterm; do "
             "command -v \"$t\" >/dev/null 2>&1 || continue; "
             "if [ \"$t\" = wezterm ]; then \"$t\" start -- sh -c '%s' & else "
             "\"$t\" -e sh -c '%s' & fi; exit 0; done",
             inner_cmd, inner_cmd);
    run_detached(cmd);
}

/* docs/14-COMPLETION-PLAN.md W5.1: tmux/zellij session list, read-only, plus
 * an "attach in terminal" action per session (the one launch this tab does --
 * attaching to an existing session mutates nothing dankc doesn't already
 * consider safe, same trust level as the launcher opening any other app). */
static void tab_mux(uictx *c)
{
    ui_section(c, "TMUX");
    mux_session tsessions[DC_MUX_MAX];
    int tn = mux_tmux_list(tsessions, DC_MUX_MAX);
    if (tn == 0) {
        bool installed = system("command -v tmux >/dev/null 2>&1") == 0;
        ui_hint(c, installed ? "No tmux sessions running" : "tmux not installed");
    } else {
        for (int i = 0; i < tn; i++) {
            char status[32];
            snprintf(status, sizeof(status), "%d window%s", tsessions[i].windows,
                     tsessions[i].windows == 1 ? "" : "s");
            if (ui_list_row(c, tsessions[i].name, status, IC_MUX, false) == 1) {
                char name[DC_MUX_NAME_MAX];
                copy_trunc(name, sizeof(name), tsessions[i].name);
                char cmd[DC_MUX_CMD_MAX];
                snprintf(cmd, sizeof(cmd), "tmux attach -t '%s'", name);
                mux_launch_terminal(cmd);
            }
        }
    }

    ui_section(c, "ZELLIJ");
    mux_session zsessions[DC_MUX_MAX];
    int zn = mux_zellij_list(zsessions, DC_MUX_MAX);
    if (zn == 0) {
        bool installed = system("command -v zellij >/dev/null 2>&1") == 0;
        ui_hint(c, installed ? "No zellij sessions running" : "zellij not installed");
    } else {
        for (int i = 0; i < zn; i++) {
            if (ui_list_row(c, zsessions[i].name, NULL, IC_MUX, false) == 1) {
                char name[DC_MUX_NAME_MAX];
                copy_trunc(name, sizeof(name), zsessions[i].name);
                char cmd[DC_MUX_CMD_MAX];
                snprintf(cmd, sizeof(cmd), "zellij attach '%s'", name);
                mux_launch_terminal(cmd);
            }
        }
    }
    ui_hint(c, "Read-only list; click a session to attach in a new terminal.");
}

/* docs/29-SMALL-FEATURES-PLAN.md sec.4 (updater T1): generalized multi-
 * backend view on top of services/updates.h (dc_updates_check_async/_read/
 * _total/_run_upgrade), replacing this tab's old Arch-only inline
 * checkupdates cache-file logic (moved into services/updates.c). Backend
 * checks are still detached/fire-and-forget and polled from a cache file --
 * updates.c owns that now, this tab just reads the summarized result. */
static const char *update_backend_label(const char *name)
{
    if (!strcmp(name, "pacman"))
        return "Pacman (official repos)";
    if (!strcmp(name, "aur"))
        return "AUR";
    if (!strcmp(name, "flatpak"))
        return "Flatpak";
    return name;
}

static void tab_system_updater(uictx *c)
{
    dc_config *cfg = c->cfg;
    dc_settings *s = c->s;

    ui_section(c, "PACKAGE UPDATES");

    int total = dc_updates_total();
    char totalbuf[16];
    snprintf(totalbuf, sizeof(totalbuf), "%d", total);
    ui_value(c, "Total pending", totalbuf);

    if (ui_list_row(c, "Check for updates", NULL, IC_SYSTEM_UPDATER, false) == 1)
        dc_updates_check_async();

    dc_update_backend backends[DC_UPDATES_BACKENDS_N];
    int n = dc_updates_read(backends, DC_UPDATES_BACKENDS_N);
    for (int i = 0; i < n; i++) {
        const dc_update_backend *b = &backends[i];
        char status[64];
        if (!b->available) {
            snprintf(status, sizeof(status), "not installed");
        } else if (b->count < 0) {
            snprintf(status, sizeof(status), "not checked");
        } else if (b->mtime > 0) {
            char when[16];
            time_t mt = (time_t)b->mtime;
            struct tm tm;
            localtime_r(&mt, &tm);
            strftime(when, sizeof(when), "%H:%M", &tm);
            snprintf(status, sizeof(status), "%d pending @ %s", b->count, when);
        } else {
            snprintf(status, sizeof(status), "%d pending", b->count);
        }
        bool actionable = b->available && b->count > 0;
        int r = ui_list_row(c, update_backend_label(b->name), status,
                            actionable ? IC_MUX : 0, false);
        if (r == 2)
            dc_updates_run_upgrade(b->name);
    }
    ui_hint(c, "\"Update in terminal\" opens an interactive terminal to run the real");
    ui_hint(c, "upgrade -- there's no embedded live-log popout here (yet).");
    ui_hint(c, "AUR checking needs paru or yay installed; Flatpak needs the flatpak CLI.");

    ui_section(c, "UPGRADE TERMINAL");
    bool tf_focus = s->focus_field == 17;
    char tfbuf[256];
    if (tf_focus)
        snprintf(tfbuf, sizeof(tfbuf), "%s", s->edit_buf);
    else
        copy_trunc(tfbuf, sizeof(tfbuf), cfg->update_terminal_cmd);
    if (ui_textfield(c, "Custom terminal command", tfbuf, tf_focus)) {
        s->focus_field = 17;
        copy_trunc(s->edit_buf, sizeof(s->edit_buf), cfg->update_terminal_cmd);
    }
    ui_hint(c, "Template with one literal %s = the upgrade command, e.g.");
    ui_hint(c, "foot -e sh -c '%s; read -p \"[enter to close]\" x' -- empty auto-probes a");
    ui_hint(c, "terminal (foot/kitty/alacritty/wezterm/ghostty/xterm).");

    ui_section(c, "AUTO-CHECK");
    if (ui_stepper(c, "Check interval (minutes, 0 = manual)", &cfg->updates_check_interval_min, 0,
                   1440, 5))
        c->changed = true;
}

/* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.6: CUPS printers, now backed by
 * services/printers.h (dc_printers_list()/set_default()/test_page()/jobs())
 * instead of this tab's own inline `lpstat -p` popen() parser -- both writes
 * are per-user, no root (see printers.h's file header). Adding/removing
 * queues (`lpadmin`) is still out of scope -- punt to system-config-printer
 * or the CUPS web UI, same as before. */
static void tab_printer(uictx *c)
{
    ui_section(c, "PRINTERS");
    if (!dc_printers_available()) {
        ui_hint(c, "CUPS not running (no `lpstat` on PATH, or no daemon).");
        ui_hint(c, "Install/start cups, or manage printers via");
        ui_hint(c, "system-config-printer / http://localhost:631.");
        return;
    }

    dc_printer_info printers[DC_PRINTERS_MAX];
    int n = dc_printers_list(printers);
    if (n == 0) {
        ui_hint(c, "No printers configured (CUPS has no queues set up).");
    } else {
        for (int i = 0; i < n; i++) {
            const dc_printer_info *p = &printers[i];
            char status[DC_PRINTER_TEXT_MAX + 16];
            snprintf(status, sizeof(status), "%s%s", p->is_default ? "Default \xe2\x80\xa2 " : "",
                    dc_printer_state_name(p->state));
            ui_list_row(c, p->name, status, 0, p->is_default);
            if (p->description[0] || p->location[0]) {
                char meta[2 * DC_PRINTER_TEXT_MAX];
                snprintf(meta, sizeof(meta), "%s%s%s", p->description,
                        (p->description[0] && p->location[0]) ? " -- " : "", p->location);
                ui_hint(c, meta);
            }
            if (!p->is_default && ui_list_row(c, "Set as default", NULL, IC_DONE, false) == 1)
                dc_printers_set_default(p->name);
            if (ui_list_row(c, "Print test page", NULL, IC_PRINTER, false) == 1)
                dc_printers_test_page(p->name);
        }
    }

    ui_section(c, "JOB QUEUE");
    dc_printer_job jobs[DC_PRINTER_JOBS_MAX];
    int jn = dc_printers_jobs(NULL, jobs);
    if (jn == 0) {
        ui_hint(c, "No active jobs");
    } else {
        for (int i = 0; i < jn; i++) {
            char val[64 + DC_PRINTER_TEXT_MAX];
            snprintf(val, sizeof(val), "%s -- %s", jobs[i].user, jobs[i].info);
            ui_value(c, jobs[i].id, val);
        }
    }

    ui_hint(c, "Adding/removing queues needs system-config-printer or");
    ui_hint(c, "the CUPS web UI (http://localhost:631).");
}

/* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.5: Firewall -- thin view over
 * services/firewall.c's ufw/firewalld dual backend. Per-service allow/deny is
 * fire-and-forget (the backend exposes no per-service *query*, only ufw/
 * firewall-cmd's own mutating verbs), so each common service gets a 2-way
 * segmented control (Allow/Deny) with no persistent "current" selection
 * rather than a toggle that would have to lie about its initial state. */
static void tab_firewall(uictx *c)
{
    ui_section(c, "FIREWALL");
    dc_firewall_info info;
    bool have = dc_firewall_status(&info);
    if (!have || !info.available) {
        ui_hint(c, "No firewall backend detected (install ufw or firewalld).");
        ui_hint(c, "dankc supports both -- whichever is present is used");
        ui_hint(c, "automatically, no configuration needed here.");
        return;
    }

    ui_value(c, "Backend", dc_firewall_backend_name(info.backend));

    static opt_flip enabled_flip;
    bool on = flip_get(&enabled_flip, info.enabled_known && info.enabled);
    if (ui_toggle(c, "Firewall enabled", "Block unsolicited incoming connections", on)) {
        dc_firewall_set_enabled(!on);
        flip_set(&enabled_flip, !on);
    }

    if (info.backend == DC_FIREWALL_BACKEND_UFW && info.default_policy[0])
        ui_value(c, "Default policy", info.default_policy);
    if (info.backend == DC_FIREWALL_BACKEND_FIREWALLD && info.active_zone[0])
        ui_value(c, "Active zone", info.active_zone);

    ui_section(c, "COMMON SERVICES");
    static const char *const allow_deny[2] = {"Allow", "Deny"};
    for (int i = 0; i < DC_FIREWALL_COMMON_SERVICE_COUNT; i++) {
        int clicked = ui_segmented(c, dc_firewall_common_services[i], allow_deny, 2, -1);
        if (clicked >= 0)
            dc_firewall_allow(dc_firewall_common_services[i], clicked == 0);
    }
    ui_hint(c, "Allow/Deny apply immediately (pkexec authenticates via dankc's");
    ui_hint(c, "own polkit agent); there's no live per-service query to show a");
    ui_hint(c, "current state, so these buttons don't reflect existing rules.");
}

/* docs/19-SETTINGS-COMPLETENESS-PLAN.md sec.7: Mouse/Touchpad/Keyboard --
 * config.json holds the toggle states (so the UI has something to read back
 * on the next Settings open), and every change immediately rewrites the niri
 * fragment via services/niri_input.c (dankc-input.kdl, included from the
 * user's real config.kdl once) so it takes effect the moment niri reloads
 * its config (niri watches config.kdl and its includes for changes). */
static void input_persist(const dc_config *cfg)
{
    dc_niri_input_config nc = {
            .touchpad_tap = cfg->input_touchpad_tap,
            .touchpad_natural_scroll = cfg->input_touchpad_natural_scroll,
            .touchpad_dwt = cfg->input_touchpad_dwt,
            .touchpad_disabled_on_external_mouse =
                    cfg->input_touchpad_disabled_on_external_mouse,
            .touchpad_accel_enabled = cfg->input_touchpad_accel_enabled,
            .touchpad_accel_speed = cfg->input_touchpad_accel_speed,
            .mouse_natural_scroll = cfg->input_mouse_natural_scroll,
            .mouse_accel_enabled = cfg->input_mouse_accel_enabled,
            .mouse_accel_speed = cfg->input_mouse_accel_speed,
            .keyboard_numlock = cfg->input_keyboard_numlock,
            .keyboard_layout = cfg->input_keyboard_layout[0] ? cfg->input_keyboard_layout : NULL,
    };
    dc_niri_input_persist(&nc, NULL);
}

static void tab_input(uictx *c)
{
    ui_section(c, "TOUCHPAD");
    if (ui_toggle(c, "Tap to click", NULL, c->cfg->input_touchpad_tap)) {
        c->cfg->input_touchpad_tap = !c->cfg->input_touchpad_tap;
        c->changed = true;
        input_persist(c->cfg);
    }
    if (ui_toggle(c, "Natural scrolling", "Content follows finger movement",
                  c->cfg->input_touchpad_natural_scroll)) {
        c->cfg->input_touchpad_natural_scroll = !c->cfg->input_touchpad_natural_scroll;
        c->changed = true;
        input_persist(c->cfg);
    }
    if (ui_toggle(c, "Disable while typing", NULL, c->cfg->input_touchpad_dwt)) {
        c->cfg->input_touchpad_dwt = !c->cfg->input_touchpad_dwt;
        c->changed = true;
        input_persist(c->cfg);
    }
    if (ui_toggle(c, "Disable when a mouse is plugged in", NULL,
                  c->cfg->input_touchpad_disabled_on_external_mouse)) {
        c->cfg->input_touchpad_disabled_on_external_mouse =
                !c->cfg->input_touchpad_disabled_on_external_mouse;
        c->changed = true;
        input_persist(c->cfg);
    }
    if (ui_toggle(c, "Custom pointer speed", "Otherwise niri's own default is used",
                  c->cfg->input_touchpad_accel_enabled)) {
        c->cfg->input_touchpad_accel_enabled = !c->cfg->input_touchpad_accel_enabled;
        c->changed = true;
        input_persist(c->cfg);
    }
    if (c->cfg->input_touchpad_accel_enabled) {
        char v[16];
        snprintf(v, sizeof(v), "%.2f", (double)c->cfg->input_touchpad_accel_speed);
        if (ui_slider(c, "Touchpad speed", &c->cfg->input_touchpad_accel_speed, -1.0f, 1.0f, v)) {
            c->changed = true;
            input_persist(c->cfg);
        }
    }

    ui_section(c, "MOUSE");
    if (ui_toggle(c, "Natural scrolling", NULL, c->cfg->input_mouse_natural_scroll)) {
        c->cfg->input_mouse_natural_scroll = !c->cfg->input_mouse_natural_scroll;
        c->changed = true;
        input_persist(c->cfg);
    }
    if (ui_toggle(c, "Custom pointer speed", "Otherwise niri's own default is used",
                  c->cfg->input_mouse_accel_enabled)) {
        c->cfg->input_mouse_accel_enabled = !c->cfg->input_mouse_accel_enabled;
        c->changed = true;
        input_persist(c->cfg);
    }
    if (c->cfg->input_mouse_accel_enabled) {
        char v[16];
        snprintf(v, sizeof(v), "%.2f", (double)c->cfg->input_mouse_accel_speed);
        if (ui_slider(c, "Mouse speed", &c->cfg->input_mouse_accel_speed, -1.0f, 1.0f, v)) {
            c->changed = true;
            input_persist(c->cfg);
        }
    }

    ui_section(c, "KEYBOARD");
    if (ui_toggle(c, "Enable Num Lock at startup", NULL, c->cfg->input_keyboard_numlock)) {
        c->cfg->input_keyboard_numlock = !c->cfg->input_keyboard_numlock;
        c->changed = true;
        input_persist(c->cfg);
    }
    bool layout_focus = c->s->focus_field == 11;
    char layoutbuf[32];
    if (layout_focus)
        copy_trunc(layoutbuf, sizeof(layoutbuf), c->s->edit_buf);
    else
        snprintf(layoutbuf, sizeof(layoutbuf), "%s", c->cfg->input_keyboard_layout);
    if (ui_textfield(c, "Keyboard layout (xkb, e.g. \"us\" or \"us,ru\")", layoutbuf,
                     layout_focus)) {
        c->s->focus_field = 11;
        snprintf(c->s->edit_buf, sizeof(c->s->edit_buf), "%s", c->cfg->input_keyboard_layout);
    }
    ui_hint(c, "Leave blank to keep niri's own configured/default layout.");

    ui_section(c, "APPLY");
    dc_niri_input_validate_result vr = dc_niri_input_last_validate();
    if (vr == DC_NIRI_INPUT_VALIDATE_FAILED)
        ui_hint(c, "niri validate reported a problem with the written config -- check logs.");
    else if (vr == DC_NIRI_INPUT_VALIDATE_OK)
        ui_hint(c, "Written to ~/.config/niri/dankc-input.kdl -- niri validate passed.");
    else
        ui_hint(c, "Written to ~/.config/niri/dankc-input.kdl -- applies on niri's next reload.");
}

/* docs/14-COMPLETION-PLAN.md W5.4: current-user info, read-only (lowest
 * priority in the whole plan -- mostly irrelevant on a single-user desktop,
 * per the task). No user-switching UI: dankc has no greeter/multi-seat
 * story (see docs/14's "Deliberately out of scope" > Greeter). */
static void tab_users(uictx *c)
{
    ui_section(c, "CURRENT USER");
    struct passwd *pw = getpwuid(getuid());
    ui_value(c, "Username", pw && pw->pw_name ? pw->pw_name : "(unknown)");
    char uidbuf[16];
    snprintf(uidbuf, sizeof(uidbuf), "%d", (int)getuid());
    ui_value(c, "UID", uidbuf);
    ui_value(c, "Home directory", pw && pw->pw_dir ? pw->pw_dir : "(unknown)");
    ui_value(c, "Shell", pw && pw->pw_shell ? pw->pw_shell : "(unknown)");
    char hostname[256] = "(unknown)";
    gethostname(hostname, sizeof(hostname) - 1);
    ui_value(c, "Hostname", hostname);
    ui_hint(c, "Single-user desktop -- read-only; dankc has no user-switching");
    ui_hint(c, "or account-management UI (see a real DE's Settings for that).");
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
    case TAB_NIGHTLIGHT:
        tab_nightlight(c);
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
    case TAB_LOCKSCREEN:
        tab_lockscreen(c);
        break;
    case TAB_WINDOW_RULES:
        tab_window_rules(c);
        break;
    case TAB_KEYBINDS:
        tab_keybinds(c);
        break;
    case TAB_MUX:
        tab_mux(c);
        break;
    case TAB_SYSTEM_UPDATER:
        tab_system_updater(c);
        break;
    case TAB_PRINTER:
        tab_printer(c);
        break;
    case TAB_FIREWALL:
        tab_firewall(c);
        break;
    case TAB_INPUT:
        tab_input(c);
        break;
    case TAB_USERS:
        tab_users(c);
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
    case 6: /* niri window-rules "add" app-id draft -- not config.json state, see
             * wr_add_rule() (writes ~/.config/niri/dankc-rules.kdl instead) */
        copy_trunc(s->wr_new_app_id, sizeof(s->wr_new_app_id), s->edit_buf);
        break;
    case 7: { /* nightlight custom-schedule "from" HH:MM -- nightlight.c owns
               * persistence itself, so this calls straight into the service
               * rather than mutating cfg (the dc_config_save() below is a
               * harmless no-op extra for this field, same as case 6). */
        char from[6], to[6];
        dc_nightlight_get_times(from, sizeof(from), to, sizeof(to));
        char newfrom[6];
        copy_trunc(newfrom, sizeof(newfrom), s->edit_buf);
        dc_nightlight_set_schedule(DC_NIGHTLIGHT_SCHED_TIMES, newfrom, to);
        break;
    }
    case 8: { /* nightlight custom-schedule "to" HH:MM */
        char from[6], to[6];
        dc_nightlight_get_times(from, sizeof(from), to, sizeof(to));
        char newto[6];
        copy_trunc(newto, sizeof(newto), s->edit_buf);
        dc_nightlight_set_schedule(DC_NIGHTLIGHT_SCHED_TIMES, from, newto);
        break;
    }
    case 9: /* network hotspot SSID draft -- not persisted until "Start
             * Hotspot" is clicked (tab_network), see dc_settings.net_hotspot_ssid */
        copy_trunc(s->net_hotspot_ssid, sizeof(s->net_hotspot_ssid), s->edit_buf);
        break;
    case 10: /* network hotspot password draft */
        copy_trunc(s->net_hotspot_password, sizeof(s->net_hotspot_password), s->edit_buf);
        break;
    case 11: /* niri input tab keyboard xkb layout */
        copy_trunc(cfg->input_keyboard_layout, sizeof(cfg->input_keyboard_layout), s->edit_buf);
        input_persist(cfg);
        break;
    case 12: /* Time & Date tab timezone-picker filter draft -- not config.json
              * state, see dc_settings.tz_filter */
        copy_trunc(s->tz_filter, sizeof(s->tz_filter), s->edit_buf);
        break;
    case 13: /* Keybinds tab "add bind" custom-command draft -- not config.json
              * state, see dc_settings.kb_custom_cmd (docs/23-KEYBIND-EDITING-
              * PLAN.md, KB-T3) */
        copy_trunc(s->kb_custom_cmd, sizeof(s->kb_custom_cmd), s->edit_buf);
        break;
    case 14: /* Keybinds tab "add bind" description (hotkey-overlay-title)
              * draft, see dc_settings.kb_title */
        copy_trunc(s->kb_title, sizeof(s->kb_title), s->edit_buf);
        break;
    case 15: /* Audio tab device rename (docs/25-AUDIO-PERDEVICE-PLAN.md T4) --
              * target node.name is s->audio_rename_target, set when the
              * textfield gained focus; empty clears the alias back to the
              * device's raw description (dc_config_audio_set_alias() already
              * treats an empty/NULL alias as "remove the entry"). */
        dc_config_audio_set_alias(s->audio_rename_target,
                                  s->edit_buf[0] ? s->edit_buf : NULL);
        break;
    case 16: /* Notifications tab "add rule" draft (docs/26-DND-SCHEDULING-
              * PLAN.md UI, DND T4) -- dedup'd case-insensitively against
              * existing rules' match strings, new entries default to
              * mute/keep (dc_notif_rule's zero-init action + urgency=-1). */
        if (s->edit_buf[0] && cfg->notif_rules_n < DC_CONFIG_NOTIF_RULES_MAX &&
            !notif_rule_match_exists(cfg->notif_rules, cfg->notif_rules_n, s->edit_buf)) {
            dc_notif_rule *r = &cfg->notif_rules[cfg->notif_rules_n];
            *r = (dc_notif_rule){0};
            r->urgency = -1;
            copy_trunc(r->match, sizeof(r->match), s->edit_buf);
            cfg->notif_rules_n++;
        }
        break;
    case 17: /* System Updater tab's "Custom terminal command" field
              * (update_terminal_cmd) -- empty clears the override back to
              * dc_updates_run_upgrade()'s auto-probe. */
        copy_trunc(cfg->update_terminal_cmd, sizeof(cfg->update_terminal_cmd), s->edit_buf);
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
    const float x0 = sp_pad_side(), w = DC_SIDEBAR_W;
    const float pad_top = sp_pad_top(), pad_bottom = sp_pad_bottom();
    nvgBeginPath(vg);
    nvgRoundedRect(vg, x0, pad_top, w, (float)s->logical_height - pad_top - pad_bottom, 16.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    nvgFontFaceId(vg, s->render->font_ui);
    nvgFontSize(vg, 20.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, x0 + 16.0f, pad_top + 28.0f, "Settings", NULL);

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
    const float w = s->logical_width, h = s->logical_height;
    const bool bottom_bar = sp_bottom_bar();
    const float pad_side = sp_pad_side();
    const float pad_top = sp_pad_top();
    const float pad_bottom = sp_pad_bottom();

    glViewport(0, 0, s->phys_width, s->phys_height);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    nvgBeginFrame(vg, w, h, (float)s->scale120 / DC_SCALE_BASE);

    float p = dc_anim_progress(&s->anim);
    if (s->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.94f + 0.06f * p;
    float ox = pad_side + (w - 2.0f * pad_side) * s->anim_ox;
    float oy = pad_top + (h - pad_top - pad_bottom) * s->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Card chrome: shadow + fill + outline, floating (flat pad 6, rounded
     * corners) or stitched into the bar (square near corners, connector
     * fillets, scissored shadow, 3-side outline) depending on
     * connected_frame -- see ui/connected.h. Note: this converges settings'
     * floating-mode chrome onto the same look every other popout already
     * uses (corner radius 16->12, shadow blur 20->18/alpha 110->90, and a
     * 1px outline that settings' old inline block never drew) rather than
     * reproducing its slightly-different pre-existing constants
     * byte-for-byte -- same convergence dashboard.c's T4 conversion already
     * made (its old shadow was blur 22/alpha 100 vs the shared 18/90). */
    dc_connected_card_chrome(vg, s->render, w, h, bottom_bar);

    draw_sidebar(s, vg, t);

    /* Content header (fixed): active tab title. */
    float cl = content_left(s), cw = content_width(s), bt = body_top(s), bh = body_height(s);
    nvgFontFaceId(vg, s->render->font_ui);
    nvgFontSize(vg, 20.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFillColor(vg, tc(t->surface_text));
    nvgText(vg, cl, pad_top + 30.0f, TABS[s->active_tab].label, NULL);

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
        float track_x = w - pad_side - 6.0f;
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
    s->logical_width = width > 0 ? (int)width : sp_surface_width();
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
        /* Displays tab verification (docs/19 sec.3): its content is taller
         * than one screen, so scripted clicks on rows below the fold need
         * a pre-scroll -- DANKC_SETTINGS_SCROLL=<content pixels> sets
         * s->scroll_y directly before the clicks below run, same one-shot
         * gate as test_clicks_done. */
        const char *scroll_spec = getenv("DANKC_SETTINGS_SCROLL");
        if (scroll_spec)
            s->scroll_y = (float)atof(scroll_spec);
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
    s->logical_width = sp_surface_width();
    s->logical_height = DC_SET_HEIGHT;
    s->scale120 = DC_SCALE_BASE;
    s->wr_new_opacity = 1.0f;
    s->kb_niri_idx = -1;
    s->kb_dankc_idx = -1;
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
    sidebar_reveal_active_tab(s);
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
    s->logical_width = sp_surface_width();
    zwlr_layer_surface_v1_set_size(s->layer_surface, (uint32_t)s->logical_width, DC_SET_HEIGHT);
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
    /* A half-finished keybind chord-capture (docs/23-KEYBIND-EDITING-PLAN.md
     * sec.3, KB-T3) must not leak its zwp_keyboard_shortcuts_inhibitor_v1 if
     * the settings surface is closed/torn down mid-capture (e.g. an
     * external "toggle settings" request while "Record shortcut" is
     * active). */
    if (s->kb_capture) {
        s->kb_capture = false;
        dc_wayland_shortcuts_uninhibit(s->wl);
    }
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
    if (s->kb_capture) { /* don't keep grabbing all keyboard input through the close animation */
        s->kb_capture = false;
        dc_wayland_shortcuts_uninhibit(s->wl);
    }
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

void dc_settings_toggle_tab(dc_settings *s, dc_output *output, dc_settings_tab tab)
{
    int internal = (tab == DC_SETTINGS_TAB_UPDATER) ? TAB_SYSTEM_UPDATER : TAB_NETWORK;
    if (s->visible && !s->closing) {
        if (s->active_tab == internal) {
            s_begin_close(s);
        } else {
            s->active_tab = internal;
            sidebar_reveal_active_tab(s);
            s->scroll_y = 0;
            s_render(s);
        }
    } else {
        s->active_tab = internal;
        s_show(s, output);
    }
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
    float pad_side = sp_pad_side();
    if (x >= pad_side && x <= pad_side + DC_SIDEBAR_W) {
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

    /* A stray click while a keybind chord-capture is in flight (docs/23-
     * KEYBIND-EDITING-PLAN.md sec.3, KB-T3) isn't the expected "press the
     * shortcut" input -- treat it as an implicit cancel (uninhibit +
     * re-render) rather than let the click fall through to whatever's
     * underneath while the inhibitor is still grabbing all keyboard input. */
    if (s->kb_capture) {
        s->kb_capture = false;
        dc_wayland_shortcuts_uninhibit(s->wl);
    }

    /* Sidebar tab switch. */
    float pad_side = sp_pad_side();
    if (x >= pad_side && x <= pad_side + DC_SIDEBAR_W) {
        const float item_h = DC_SIDEBAR_ITEM_H, top = sidebar_body_top(s);
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
    return s->visible && !s->closing && (s->focus_field != 0 || s->kb_capture);
}

void dc_settings_handle_key(dc_settings *s, uint32_t keysym, const char *utf8)
{
    /* Keybind chord capture (docs/23-KEYBIND-EDITING-PLAN.md sec.3, KB-T3)
     * consumes every key while active -- it must run before the
     * focus_field-based text editing below (they're mutually exclusive:
     * kb_capture_key() never leaves focus_field set). */
    if (s->kb_capture) {
        kb_capture_key(s, keysym);
        return;
    }
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
