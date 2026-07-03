#include "ui/controlcenter.h"

#include "core/anim.h"
#include "core/config.h"
#include "core/log.h"
#include "dc.h"
#include "render/icons.h"
#include "render/nvg.h"
#include "render/shape.h"
#include "services/audio.h"
#include "services/bluez.h"
#include "services/mpris.h"
#include "services/net.h"
#include "theme/theme.h"
#include "ui/bar/bar_tokens.h"
#include "ui/hover.h"
#include "ui/material_bg.h"
#include "ui/popout.h"
#include "wayland/egl.h"
#include "wayland/wl.h"

#include <GLES2/gl2.h>
#include <ctype.h>
#include <dirent.h>
#include <limits.h>
#include <math.h>
#include <pwd.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>

#include "fractional-scale-v1-client-protocol.h"
#include "nanovg.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

/* Width/height picked to match the user's live DMS reference screenshot
 * (~/Pictures/Screenshots/Screenshot from 2026-07-02 14-17-26.png, itself
 * ~541x350 for the popout alone) and DMS's ControlCenterPopout.qml
 * `popupWidth: 550` -- dankc's other popouts (launcher 600, clip_picker 640,
 * settings 520) already use QML popupWidth-ish numbers directly as logical
 * px with no extra conversion factor, so this follows the same convention
 * rather than the old 380x420 (sized for the previous 2x2-tiles layout). */
#define DC_CC_WIDTH 480
#define DC_CC_HEIGHT 372
#define DC_SCALE_BASE 120
/* Inset from the screen's right edge when bar-adjacent (docs/13-POPOUTS-SPEC.md
 * sec.0/1: opens near the bar's right cluster, a few px in from the edge). */
#define DC_CC_SIDE_MARGIN 12

/* Max visible rows in the network/bluetooth expand panel -- defined ahead of
 * cc_hover_id below so the enum can size CC_HOVER_EXPAND_ROW_BASE's reserved
 * block of ids from it. */
#define CC_MAX_EXPAND_ROWS 5

/* Hover/hit ids for the tiles/buttons/sliders (docs/13-POPOUTS-SPEC.md sec.1:
 * hover bg on tiles + header action buttons, matching bar.c's per-widget hit
 * region convention). CC_HOVER_NONE must stay 0 (default-initialized state =
 * nothing hovered). */
typedef enum {
    CC_HOVER_NONE = 0,
    CC_HOVER_BTN_LOCK,
    CC_HOVER_BTN_POWER,
    CC_HOVER_BTN_SETTINGS,
    CC_HOVER_BTN_EDIT,
    CC_HOVER_SLIDER_VOLUME,
    CC_HOVER_SLIDER_BRIGHTNESS,
    CC_HOVER_TILE_0_0,
    CC_HOVER_TILE_0_1,
    CC_HOVER_TILE_1_0,
    CC_HOVER_TILE_1_1,
    CC_HOVER_TILE_2_0,
    CC_HOVER_TILE_2_1,
    /* Wi-Fi/bluetooth tile expand chevrons (docs/13-POPOUTS-SPEC.md sec.1
     * item 1/2: expandable network/bluetooth sections). */
    CC_HOVER_WIFI_CHEVRON,
    CC_HOVER_BT_CHEVRON,
    /* Media transport row (shown only while an MPRIS player is active). */
    CC_HOVER_MEDIA_PREV,
    CC_HOVER_MEDIA_PLAY,
    CC_HOVER_MEDIA_NEXT,
    /* Rows of whichever expand panel (network or bluetooth) is currently
     * open -- CC_MAX_EXPAND_ROWS consecutive ids starting here, one per
     * visible row (mutually exclusive with the other panel, so the id alone
     * plus cc->net_expanded/bt_expanded disambiguates which list a row hit
     * belongs to). */
    CC_HOVER_EXPAND_ROW_BASE,
    /* Inline Wi-Fi password entry (W1.1), reserved right after the expand
     * rows' block so the two never collide. */
    CC_HOVER_NET_PW_FIELD = CC_HOVER_EXPAND_ROW_BASE + CC_MAX_EXPAND_ROWS,
    CC_HOVER_NET_PW_CANCEL,
    CC_HOVER_NET_PW_CONNECT,
    /* Bluetooth "Discover" toggle + inline pairing-agent panel (W3.1),
     * reusing the exact inline-field pattern above. */
    CC_HOVER_BT_SCAN_BTN,
    CC_HOVER_BT_AGENT_FIELD,
    CC_HOVER_BT_AGENT_NO,
    CC_HOVER_BT_AGENT_YES,
    /* Adapter "Discoverable" toggle (W1 polish, docs/18-WIFI-BT-PLAN.md),
     * beside the Discover/Scan button on the same header row. */
    CC_HOVER_BT_DISCOVERABLE_BTN,
    /* Per-row action icons, CC_MAX_EXPAND_ROWS consecutive ids each: Bluetooth
     * Trust/Remove (paired devices only) and Wi-Fi Forget (saved networks
     * only) -- geometry is fixed per row (right-aligned within the row rect),
     * only whether they're active/drawn depends on that row's data (see
     * cc_get_layout()'s net_row_saved/bt_row_paired params). */
    CC_HOVER_BT_TRUST_BASE,
    CC_HOVER_BT_REMOVE_BASE = CC_HOVER_BT_TRUST_BASE + CC_MAX_EXPAND_ROWS,
    CC_HOVER_NET_FORGET_BASE = CC_HOVER_BT_REMOVE_BASE + CC_MAX_EXPAND_ROWS,
} cc_hover_id;

/* Max length of the inline Wi-Fi password buffer (nmcli/WPA itself caps PSKs
 * at 63 chars; this leaves generous headroom for enterprise-style passwords
 * too). */
#define CC_NET_PW_MAX 128

/* Inline Wi-Fi password entry geometry (W1.1) -- a masked text field, a
 * one-line status slot (blank / "Connecting..." / an inline error), and a
 * Cancel/Connect button row, inserted directly below the SSID row it
 * belongs to (see cc_get_layout()'s pw_after_row handling and
 * cc_expand_row_y()). */
#define CC_NET_PW_GAP 8.0f
#define CC_NET_PW_FIELD_H 32.0f
#define CC_NET_PW_STATUS_H 16.0f
#define CC_NET_PW_BTN_H 28.0f
#define CC_NET_PW_PANEL_H                                                                       \
    (CC_NET_PW_GAP + CC_NET_PW_FIELD_H + CC_NET_PW_GAP + CC_NET_PW_STATUS_H + CC_NET_PW_BTN_H + \
     CC_NET_PW_GAP)

struct dc_control_center {
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

    dc_anim anim;
    struct wl_callback *frame_cb;
    bool closing;

    /* Entrance/exit scale-and-fade pivot, bar-position-aware (docs/13-POPOUTS-
     * SPEC.md sec.0): fraction of (w,h) nearest the bar-facing edge, set from
     * dc_popout_bar_adjacent() at show-time. */
    float anim_ox, anim_oy;

    /* Hover tracking (docs/13-POPOUTS-SPEC.md sec.1), same guard pattern as
     * bar.c's dc_bar_pointer_motion(): only re-render when the hovered id
     * actually changes. */
    cc_hover_id hover_id;

    /* Slider drag (button-held-motion; docs/13-POPOUTS-SPEC.md sec.1): press
     * inside a slider's hit rect both click-to-sets and arms this, so every
     * subsequent motion while the button stays held keeps updating the value
     * live, ending on release. `slider_drag_value` is the fraction currently
     * shown (independent of the system's actual current value, so the fill
     * tracks the pointer exactly rather than racing a wpctl/brightnessctl
     * round-trip). */
    bool slider_dragging;
    int slider_drag_slot; /* 0 = volume, 1 = brightness */
    float slider_drag_value;

    /* Expandable network/bluetooth sections (docs/13-POPOUTS-SPEC.md sec.1
     * items 1/2): mutually exclusive (opening one closes the other, so both
     * never need vertical space at once). The card grows to fit the open
     * panel's rows -- see cc_get_layout()'s total_h and the resize-on-render
     * logic in cc_render(). */
    bool net_expanded;
    bool bt_expanded;

    /* Inline Wi-Fi password entry (W1.1): clicking a secured, not-yet-known
     * SSID opens a masked password field + Connect/Cancel row directly below
     * that SSID (see cc_find_pw_row()/CC_NET_PW_* layout) instead of the old
     * dead-end "Needs Password" hint. `net_pw_connecting` is set for the
     * duration of an in-flight dc_net_wifi_connect_psk() job -- while true,
     * cc_render() keeps requesting frame callbacks purely to keep polling
     * dc_net_wifi_connect_poll() until it resolves (see the `frame_cb`
     * request at the end of cc_render()). `net_pw_err`, once non-empty,
     * shows an inline "wrong password"-style message until the user edits
     * the field again or cancels. */
    bool net_pw_active;
    bool net_pw_connecting;
    char net_pw_ssid[64];
    char net_pw_buf[CC_NET_PW_MAX];
    char net_pw_err[96];

    /* Bluetooth pairing (W3.1): `bt_pairing_active` mirrors `net_pw_connecting`
     * -- set once dc_bluez_pair() is kicked off, keeps cc_render() requesting
     * frame callbacks (see the bottom of cc_render()) purely to re-poll
     * dc_bluez_pair_poll() until the async Pair/Trust/Connect chain resolves,
     * since those replies arrive via the system bus's own event-loop
     * integration rather than anything this file drives directly.
     * `bt_passkey_buf` is the entry buffer for a RequestPasskey (digits only)
     * or RequestPinCode (any printable char, legacy pre-SSP pairing) agent
     * prompt (dc_bluez_agent_poll()'s DC_BLUEZ_AGENT_PASSKEY/PIN_CODE kinds,
     * see cc_bt_agent_has_field()) -- unlike the Wi-Fi password field,
     * whether this is even showing is entirely driven by bluez.c's agent
     * state, not a flag owned here. Sized for the legacy PIN's up to 16
     * chars (bluez.h), comfortably more than a 6-digit passkey needs. */
    bool bt_pairing_active;
    char bt_passkey_buf[24];

    bool visible;
    bool configured;
    bool egl_ready;
};

static void cc_render(dc_control_center *cc);
static void cc_teardown(dc_control_center *cc);
static bool cc_bt_agent_has_field(dc_bluez_agent_kind k);

static void cc_frame_done(void *data, struct wl_callback *cb, uint32_t time);
static const struct wl_callback_listener cc_frame_listener = {.done = cc_frame_done};

static void cc_frame_done(void *data, struct wl_callback *cb, uint32_t time)
{
    dc_control_center *cc = data;
    DC_UNUSED(time);
    wl_callback_destroy(cb);
    cc->frame_cb = NULL;
    if (!cc->visible)
        return;
    if (dc_anim_active(&cc->anim) || cc->net_pw_connecting || cc->bt_pairing_active)
        cc_render(cc);
    else if (cc->closing)
        cc_teardown(cc);
}

static inline NVGcolor tc(dc_color c)
{
    return nvgRGBA(c.r, c.g, c.b, c.a);
}

static inline NVGcolor tc_alpha(dc_color c, int a)
{
    return nvgRGBA(c.r, c.g, c.b, (unsigned char)a);
}

/* Shared layout so cc_render (draw) and handle_click (hit-test) agree.
 * Matches docs/13-POPOUTS-SPEC.md sec.1: user header card, two side-by-side
 * sliders, then a 2-column x 3-row tile grid (wifi/bluetooth, audioOutput/
 * audioInput, nightMode/darkMode). */
typedef struct {
    float ix, iw;

    float header_y, header_h;
    float avatar_cx, avatar_cy, avatar_r;
    float btn_cx[4]; /* lock, power, settings, edit */
    float btn_cy, btn_r;

    float sliders_y, slider_h;
    float slot_x[2]; /* volume, brightness */
    float slot_w;

    /* Media transport row (docs/13-POPOUTS-SPEC.md sec.1 item 4), only
     * present (media_h > 0) while an MPRIS player is active. */
    bool media_active;
    float media_y, media_h;
    float media_art_cx, media_art_cy, media_art_r;
    float media_text_x, media_text_w;
    float media_btn_cx[3]; /* prev, play/pause, next */
    float media_btn_cy, media_btn_r;

    float tiles_y0, tile_w, tile_h, gap;

    /* Expand panel (network or bluetooth list), appended below the tile
     * grid -- 0 = collapsed (expand_h == 0), 1 = network, 2 = bluetooth. */
    int expand_kind;
    int expand_rows;
    float expand_y0, expand_h;
    float expand_header_y;
    float expand_row_y0, expand_row_h;
    float wifi_chevron_cx, wifi_chevron_cy;
    float bt_chevron_cx, bt_chevron_cy;
    float chevron_r;

    /* Inline Wi-Fi password entry (W1.1): -1 when not shown, else the
     * 0-based expand-row index it's inserted directly below. Rows after it
     * are pushed down by `pw_panel_h` -- see cc_expand_row_y(). */
    int pw_after_row;
    float pw_panel_h;
    float pw_field_y, pw_field_h;
    float pw_status_y;
    float pw_btn_y0, pw_btn_h;
    float pw_cancel_x0, pw_cancel_x1;
    float pw_connect_x0, pw_connect_x1;

    /* Bluetooth "Discover" toggle (W3.1), on the BLUETOOTH DEVICES header
     * row, only meaningful when expand_kind == 2. */
    float bt_scan_btn_x0, bt_scan_btn_x1, bt_scan_btn_y0, bt_scan_btn_h;

    /* Adapter "Discoverable" toggle (W1 polish), same header row, just left
     * of the Discover/Scan button. */
    float bt_disc_btn_x0, bt_disc_btn_x1, bt_disc_btn_y0, bt_disc_btn_h;

    /* Per-row action icons (W1/W2 polish): fixed geometry (same for every
     * row, right-aligned within the row rect via cc_expand_row_y()'s y plus
     * l->expand_row_h), only drawn/hit-testable for rows the caller flagged
     * via cc_get_layout()'s net_row_saved/bt_row_paired params -- Wi-Fi
     * "Forget" for saved networks, Bluetooth Trust + Remove for paired
     * devices. */
    bool net_row_saved[CC_MAX_EXPAND_ROWS];
    float net_forget_cx, net_forget_r;
    bool bt_row_paired[CC_MAX_EXPAND_ROWS];
    float bt_trust_cx, bt_remove_cx, bt_action_r;

    /* Bluetooth pairing-agent panel (W3.1): appended after the device rows
     * (not tied to a particular row, unlike the Wi-Fi password panel above --
     * BlueZ's agent request isn't scoped to whichever row is currently
     * visible), shown whenever bluez.c reports a pending confirm/authorize/
     * passkey request while the bluetooth section is open. */
    bool bt_agent_active;
    dc_bluez_agent_kind bt_agent_kind;
    float bt_agent_h;
    float bt_agent_text_y;
    float bt_agent_field_y, bt_agent_field_h; /* only for PASSKEY/PIN_CODE, see cc_bt_agent_has_field() */
    float bt_agent_btn_y0, bt_agent_btn_h;
    float bt_agent_no_x0, bt_agent_no_x1;
    float bt_agent_yes_x0, bt_agent_yes_x1;

    /* Total content height (pad-to-pad, i.e. the card's own height) --
     * cc_render() resizes the layer-surface to this when it changes. */
    float total_h;
} cc_layout;

/* `media_active`: an MPRIS player is present (adds the transport row).
 * `expand_kind`/`expand_rows`: 0/1/2 = none/network/bluetooth and how many
 * rows that panel is currently showing (0..CC_MAX_EXPAND_ROWS) -- both
 * gathered once per render/click/motion by cc_gather_state() so every
 * layout consumer agrees on the card's current size. `pw_after_row`: -1, or
 * the 0-based network-row index the inline password panel (W1.1) is
 * currently open under (only meaningful when expand_kind == 1).
 * `bt_agent_kind`: DC_BLUEZ_AGENT_NONE, or which pairing-agent panel (W3.1)
 * to reserve room for below the bluetooth device rows (only meaningful when
 * expand_kind == 2). `net_row_saved`/`bt_row_paired`: NULL, or an array of
 * CC_MAX_EXPAND_ROWS bools flagging which rows get the Forget / Trust+Remove
 * action icons (W1/W2 polish) -- copied verbatim into the returned layout so
 * cc_hittest()/draw_cc_hover() (which don't otherwise see per-row service
 * data) can tell which rows have them. */
static cc_layout cc_get_layout(float w, bool media_active, int expand_kind, int expand_rows,
                               int pw_after_row, dc_bluez_agent_kind bt_agent_kind,
                               const bool *net_row_saved, const bool *bt_row_paired)
{
    const float pad = 6.0f;   /* room for the drop shadow */
    const float margin = 16.0f; /* content inset from the card edge (~Theme.spacingL) */
    const float gap = 8.0f;     /* ~Theme.spacingS, used between every stacked row */

    cc_layout l;
    memset(&l, 0, sizeof(l));
    l.ix = pad + margin;
    l.iw = w - 2.0f * l.ix;

    if (net_row_saved)
        memcpy(l.net_row_saved, net_row_saved, sizeof(l.net_row_saved));
    if (bt_row_paired)
        memcpy(l.bt_row_paired, bt_row_paired, sizeof(l.bt_row_paired));
    l.net_forget_r = 9.0f;
    l.net_forget_cx = l.ix + l.iw - l.net_forget_r - 4.0f;
    l.bt_action_r = 9.0f;
    l.bt_remove_cx = l.ix + l.iw - l.bt_action_r - 4.0f;
    l.bt_trust_cx = l.bt_remove_cx - 2.0f * l.bt_action_r - 6.0f;

    l.header_y = pad + margin;
    l.header_h = 70.0f;
    l.avatar_r = 30.0f;
    l.avatar_cx = l.ix + 16.0f + l.avatar_r;
    l.avatar_cy = l.header_y + l.header_h / 2.0f;
    l.btn_cy = l.avatar_cy;
    l.btn_r = 16.0f;
    /* lock/power/settings/edit, right-aligned, 40px apart center-to-center. */
    l.btn_cx[3] = l.ix + l.iw - 16.0f;
    l.btn_cx[2] = l.btn_cx[3] - 40.0f;
    l.btn_cx[1] = l.btn_cx[2] - 40.0f;
    l.btn_cx[0] = l.btn_cx[1] - 40.0f;

    l.sliders_y = l.header_y + l.header_h + gap;
    l.slider_h = 40.0f;
    l.slot_w = (l.iw - gap) / 2.0f;
    l.slot_x[0] = l.ix;
    l.slot_x[1] = l.ix + l.slot_w + gap;

    float after_sliders = l.sliders_y + l.slider_h + gap;

    l.media_active = media_active;
    if (media_active) {
        l.media_y = after_sliders;
        l.media_h = 56.0f;
        l.media_art_r = 20.0f;
        l.media_art_cx = l.ix + l.media_art_r;
        l.media_art_cy = l.media_y + l.media_h / 2.0f;
        l.media_btn_r = 15.0f;
        l.media_btn_cy = l.media_art_cy;
        l.media_btn_cx[2] = l.ix + l.iw - l.media_btn_r;               /* next */
        l.media_btn_cx[1] = l.media_btn_cx[2] - 2.0f * l.media_btn_r - 10.0f; /* play/pause */
        l.media_btn_cx[0] = l.media_btn_cx[1] - 2.0f * l.media_btn_r - 10.0f; /* prev */
        l.media_text_x = l.media_art_cx + l.media_art_r + 10.0f;
        l.media_text_w = (l.media_btn_cx[0] - l.media_btn_r - 8.0f) - l.media_text_x;
        l.tiles_y0 = l.media_y + l.media_h + gap;
    } else {
        l.tiles_y0 = after_sliders;
    }

    l.gap = gap;
    l.tile_w = (l.iw - gap) / 2.0f;
    l.tile_h = 60.0f;

    float tiles_end = l.tiles_y0 + 3.0f * l.tile_h + 2.0f * gap;

    /* Row 0 of the tile grid is wifi (col 0) / bluetooth (col 1) -- matches
     * cc_tile_x()/cc_tile_y()'s formulas (defined below; inlined here since
     * this function runs before those are declared). */
    float tile0_x = l.ix;
    float tile1_x = l.ix + l.tile_w + gap;
    float row0_y = l.tiles_y0;
    l.chevron_r = 11.0f;
    l.wifi_chevron_cx = tile0_x + l.tile_w - 16.0f;
    l.wifi_chevron_cy = row0_y + l.tile_h / 2.0f;
    l.bt_chevron_cx = tile1_x + l.tile_w - 16.0f;
    l.bt_chevron_cy = row0_y + l.tile_h / 2.0f;

    l.expand_kind = expand_kind;
    l.expand_rows = expand_rows;
    l.pw_after_row = -1;
    if (expand_kind != 0) {
        int rows = expand_rows > 0 ? expand_rows : 1; /* room for an empty-state line */
        if (rows > CC_MAX_EXPAND_ROWS)
            rows = CC_MAX_EXPAND_ROWS;
        l.expand_y0 = tiles_end + gap;
        l.expand_header_y = l.expand_y0 + 10.0f;
        l.expand_row_h = 28.0f;
        l.expand_row_y0 = l.expand_y0 + 22.0f;

        if (expand_kind == 1 && pw_after_row >= 0 && pw_after_row < rows) {
            l.pw_after_row = pw_after_row;
            l.pw_panel_h = CC_NET_PW_PANEL_H;
            float pw_top = l.expand_row_y0 + (float)(pw_after_row + 1) * l.expand_row_h;
            l.pw_field_y = pw_top + CC_NET_PW_GAP;
            l.pw_field_h = CC_NET_PW_FIELD_H;
            l.pw_status_y =
                l.pw_field_y + l.pw_field_h + CC_NET_PW_GAP * 0.5f + CC_NET_PW_STATUS_H * 0.5f;
            l.pw_btn_y0 = l.pw_field_y + l.pw_field_h + CC_NET_PW_GAP * 0.5f + CC_NET_PW_STATUS_H;
            l.pw_btn_h = CC_NET_PW_BTN_H;
            l.pw_connect_x1 = l.ix + l.iw;
            l.pw_connect_x0 = l.pw_connect_x1 - 84.0f;
            l.pw_cancel_x1 = l.pw_connect_x0 - 8.0f;
            l.pw_cancel_x0 = l.pw_cancel_x1 - 72.0f;
        }

        float bt_agent_panel_h = 0.0f;
        if (expand_kind == 2) {
            /* Discover toggle on the BLUETOOTH DEVICES header row. */
            l.bt_scan_btn_h = 20.0f;
            l.bt_scan_btn_y0 = l.expand_y0;
            l.bt_scan_btn_x1 = l.ix + l.iw;
            l.bt_scan_btn_x0 = l.bt_scan_btn_x1 - 64.0f;

            /* Adapter Discoverable toggle, same row, just left of Discover. */
            l.bt_disc_btn_h = l.bt_scan_btn_h;
            l.bt_disc_btn_y0 = l.bt_scan_btn_y0;
            l.bt_disc_btn_x1 = l.bt_scan_btn_x0 - 8.0f;
            l.bt_disc_btn_x0 = l.bt_disc_btn_x1 - 64.0f;

            if (bt_agent_kind != DC_BLUEZ_AGENT_NONE) {
                l.bt_agent_active = true;
                l.bt_agent_kind = bt_agent_kind;
                float y = l.expand_row_y0 + (float)rows * l.expand_row_h + 6.0f;
                l.bt_agent_text_y = y + 10.0f;
                float cursor = y + 22.0f;
                if (cc_bt_agent_has_field(bt_agent_kind)) {
                    l.bt_agent_field_y = cursor;
                    l.bt_agent_field_h = CC_NET_PW_FIELD_H;
                    cursor = l.bt_agent_field_y + l.bt_agent_field_h + CC_NET_PW_GAP;
                }
                l.bt_agent_btn_y0 = cursor;
                l.bt_agent_btn_h = CC_NET_PW_BTN_H;
                l.bt_agent_yes_x1 = l.ix + l.iw;
                l.bt_agent_yes_x0 = l.bt_agent_yes_x1 - 84.0f;
                l.bt_agent_no_x1 = l.bt_agent_yes_x0 - 8.0f;
                l.bt_agent_no_x0 = l.bt_agent_no_x1 - 72.0f;
                l.bt_agent_h = (l.bt_agent_btn_y0 + l.bt_agent_btn_h) - y + CC_NET_PW_GAP;
                bt_agent_panel_h = l.bt_agent_h;
            }
        }

        l.expand_h = 22.0f + (float)rows * l.expand_row_h + l.pw_panel_h + bt_agent_panel_h + 6.0f;
        l.total_h = l.expand_y0 + l.expand_h + margin + pad;
    } else {
        l.total_h = tiles_end + margin + pad;
    }

    return l;
}

static float cc_tile_x(const cc_layout *l, int col)
{
    return l->ix + (float)col * (l->tile_w + l->gap);
}

static float cc_tile_y(const cc_layout *l, int row)
{
    return l->tiles_y0 + (float)row * (l->tile_h + l->gap);
}

/* Top y of expand-panel row `i` (0-based, within whichever of net/bt is
 * currently open) -- shared by draw + hittest + click, same convention as
 * cc_tile_x()/cc_tile_y(). Rows below an open inline password panel (W1.1)
 * are pushed down by its height. */
static float cc_expand_row_y(const cc_layout *l, int i)
{
    float y = l->expand_row_y0 + (float)i * l->expand_row_h;
    if (l->pw_after_row >= 0 && i > l->pw_after_row)
        y += l->pw_panel_h;
    return y;
}

/* Reserved on the trailing edge of every slider track for its live "NN%"
 * label (docs/13-POPOUTS-SPEC.md sec.1 item 3) -- shared by draw_slider()
 * and cc_slider_track() so the label never overlaps the fill and drag
 * hit-testing still maps a pointer x to the same fraction the fill shows.
 * Must fit the widest label ("100%" at 12px Inter, ~30px) plus an 8px
 * (spacing-XS token) gap after the track's rounded end -- 32px left ~2px,
 * so "100%" visibly collided with the fill/track end. 44px gives the full
 * 8px gap at 100% with room to spare. */
#define CC_SLIDER_LABEL_W 44.0f

/* Track geometry for slider `slot` (0=volume, 1=brightness) — the fill/track
 * inset (32px, past the leading icon) is shared by draw_slider(), the click
 * handler, and the drag-motion handler, so all three agree on where a
 * pointer x maps to a fraction. */
static void cc_slider_track(const cc_layout *l, int slot, float *track_x, float *track_w)
{
    *track_x = l->slot_x[slot] + 32.0f;
    *track_w = l->slot_w - 32.0f - CC_SLIDER_LABEL_W;
}

static float cc_slider_frac_at(const cc_layout *l, int slot, double x)
{
    float track_x, track_w;
    cc_slider_track(l, slot, &track_x, &track_w);
    float frac = (float)(x - (double)track_x) / track_w;
    if (frac < 0.0f)
        frac = 0.0f;
    if (frac > 1.0f)
        frac = 1.0f;
    return frac;
}

/* Which interactive element (if any) sits under (x, y) — shared by the click
 * handler's dispatch and the hover-motion guard, so they can never disagree
 * about hit boundaries (same "one function drives measure+draw" discipline
 * as bar.c's layout_workspaces()). */
static cc_hover_id cc_hittest(const cc_layout *l, double x, double y)
{
    for (int i = 0; i < 4; i++) {
        double dx = x - (double)l->btn_cx[i];
        double dy = y - (double)l->btn_cy;
        if (dx * dx + dy * dy <= (double)(l->btn_r * l->btn_r))
            return (cc_hover_id)(CC_HOVER_BTN_LOCK + i);
    }

    for (int i = 0; i < 2; i++) {
        if (x < (double)l->slot_x[i] || x > (double)(l->slot_x[i] + l->slot_w))
            continue;
        if (y < (double)l->sliders_y || y > (double)(l->sliders_y + l->slider_h))
            continue;
        return (cc_hover_id)(CC_HOVER_SLIDER_VOLUME + i);
    }

    for (int row = 0; row < 3; row++) {
        float ry = cc_tile_y(l, row);
        if (y < (double)ry || y > (double)(ry + l->tile_h))
            continue;
        for (int col = 0; col < 2; col++) {
            float rx = cc_tile_x(l, col);
            if (x < (double)rx || x > (double)(rx + l->tile_w))
                continue;
            return (cc_hover_id)(CC_HOVER_TILE_0_0 + row * 2 + col);
        }
    }

    /* Wifi/bluetooth expand chevrons sit on top of row-0's tiles -- checked
     * after the tile grid above so a chevron hit wins over the tile it
     * overlaps (matches the click handler's own ordering). */
    {
        double dx = x - (double)l->wifi_chevron_cx, dy = y - (double)l->wifi_chevron_cy;
        if (dx * dx + dy * dy <= (double)(l->chevron_r * l->chevron_r))
            return CC_HOVER_WIFI_CHEVRON;
        dx = x - (double)l->bt_chevron_cx;
        dy = y - (double)l->bt_chevron_cy;
        if (dx * dx + dy * dy <= (double)(l->chevron_r * l->chevron_r))
            return CC_HOVER_BT_CHEVRON;
    }

    if (l->media_active) {
        for (int i = 0; i < 3; i++) {
            double dx = x - (double)l->media_btn_cx[i], dy = y - (double)l->media_btn_cy;
            if (dx * dx + dy * dy <= (double)(l->media_btn_r * l->media_btn_r))
                return (cc_hover_id)(CC_HOVER_MEDIA_PREV + i);
        }
    }

    /* Bluetooth "Discover"/"Discoverable" toggles (W3.1/W1) -- on the header
     * row, above the device-row band, so this is safe to check before the
     * row loop. */
    if (l->expand_kind == 2) {
        if (x >= (double)l->bt_scan_btn_x0 && x <= (double)l->bt_scan_btn_x1 &&
            y >= (double)l->bt_scan_btn_y0 && y <= (double)(l->bt_scan_btn_y0 + l->bt_scan_btn_h))
            return CC_HOVER_BT_SCAN_BTN;
        if (x >= (double)l->bt_disc_btn_x0 && x <= (double)l->bt_disc_btn_x1 &&
            y >= (double)l->bt_disc_btn_y0 && y <= (double)(l->bt_disc_btn_y0 + l->bt_disc_btn_h))
            return CC_HOVER_BT_DISCOVERABLE_BTN;
    }

    if (l->expand_kind != 0 && l->expand_rows > 0) {
        int rows = l->expand_rows > CC_MAX_EXPAND_ROWS ? CC_MAX_EXPAND_ROWS : l->expand_rows;
        for (int i = 0; i < rows; i++) {
            float ry = cc_expand_row_y(l, i);
            if (y < (double)ry || y > (double)(ry + l->expand_row_h))
                continue;
            float cy = ry + l->expand_row_h / 2.0f;

            /* Per-row action icons (W1/W2 polish) -- checked before the
             * generic full-row rect below so they take priority over the
             * "tap a row to connect/pair" action they sit on top of. */
            if (l->expand_kind == 1 && l->net_row_saved[i]) {
                double dx = x - (double)l->net_forget_cx, dy = y - (double)cy;
                if (dx * dx + dy * dy <= (double)(l->net_forget_r * l->net_forget_r))
                    return (cc_hover_id)(CC_HOVER_NET_FORGET_BASE + i);
            } else if (l->expand_kind == 2 && l->bt_row_paired[i]) {
                double dx = x - (double)l->bt_trust_cx, dy = y - (double)cy;
                if (dx * dx + dy * dy <= (double)(l->bt_action_r * l->bt_action_r))
                    return (cc_hover_id)(CC_HOVER_BT_TRUST_BASE + i);
                dx = x - (double)l->bt_remove_cx;
                if (dx * dx + dy * dy <= (double)(l->bt_action_r * l->bt_action_r))
                    return (cc_hover_id)(CC_HOVER_BT_REMOVE_BASE + i);
            }

            if (x < (double)l->ix || x > (double)(l->ix + l->iw))
                continue;
            return (cc_hover_id)(CC_HOVER_EXPAND_ROW_BASE + i);
        }
    }

    /* Inline Wi-Fi password entry (W1.1): field + Cancel/Connect buttons,
     * checked after the row loop above since the panel sits in the gap
     * between two (possibly shifted) rows, not over any row's own rect. */
    if (l->pw_after_row >= 0) {
        if (x >= (double)l->ix && x <= (double)(l->ix + l->iw) && y >= (double)l->pw_field_y &&
            y <= (double)(l->pw_field_y + l->pw_field_h))
            return CC_HOVER_NET_PW_FIELD;
        if (y >= (double)l->pw_btn_y0 && y <= (double)(l->pw_btn_y0 + l->pw_btn_h)) {
            if (x >= (double)l->pw_cancel_x0 && x <= (double)l->pw_cancel_x1)
                return CC_HOVER_NET_PW_CANCEL;
            if (x >= (double)l->pw_connect_x0 && x <= (double)l->pw_connect_x1)
                return CC_HOVER_NET_PW_CONNECT;
        }
    }

    /* Bluetooth pairing-agent panel (W3.1): field (passkey/PIN entry only) +
     * Reject/Confirm (or Cancel/Submit) buttons. */
    if (l->bt_agent_active) {
        if (cc_bt_agent_has_field(l->bt_agent_kind) && x >= (double)l->ix &&
            x <= (double)(l->ix + l->iw) && y >= (double)l->bt_agent_field_y &&
            y <= (double)(l->bt_agent_field_y + l->bt_agent_field_h))
            return CC_HOVER_BT_AGENT_FIELD;
        if (y >= (double)l->bt_agent_btn_y0 && y <= (double)(l->bt_agent_btn_y0 + l->bt_agent_btn_h)) {
            if (x >= (double)l->bt_agent_no_x0 && x <= (double)l->bt_agent_no_x1)
                return CC_HOVER_BT_AGENT_NO;
            if (x >= (double)l->bt_agent_yes_x0 && x <= (double)l->bt_agent_yes_x1)
                return CC_HOVER_BT_AGENT_YES;
        }
    }

    return CC_HOVER_NONE;
}

/* Run a shell command detached (children auto-reaped via SIG_IGN on SIGCHLD). */
static void run_detached(const char *cmd)
{
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
}

/* Invoke `dankc ctl <subcmd>` against the running instance's control socket
 * (the same path `dankc keybinds`' niri snippet and dankctl use) so the
 * header's lock/settings buttons reuse the existing lock-screen/settings-panel
 * mechanisms instead of reimplementing them here (docs/13-POPOUTS-SPEC.md
 * sec.1). Resolved via /proc/self/exe rather than relying on `dankc` being on
 * PATH, since dev.sh/tests often run ./bin/dankc directly. */
static void run_self_ctl(const char *subcmd)
{
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    const char *path = "dankc";
    if (n > 0) {
        exe[n] = '\0';
        path = exe;
    }
    pid_t pid = fork();
    if (pid == 0) {
        setsid();
        execl(path, path, "ctl", subcmd, (char *)NULL);
        _exit(127);
    }
}

/* Apply a 0..1 volume fraction via the audio service's setter (docs/13-
 * POPOUTS-SPEC.md sec.1 slider drag) -- shared by click-to-set and every
 * motion event of a live drag. */
static void cc_apply_volume_frac(float frac)
{
    dc_audio_set_volume((int)(frac * 100.0f + 0.5f));
}

/* Apply a 0..1 brightness fraction via the existing brightnessctl path
 * (unchanged from the pre-hover click-to-set implementation; audio.h grew a
 * setter for this task but backlight has no equivalent service yet, so this
 * stays a direct run_detached() like before). */
static void cc_apply_brightness_frac(float frac)
{
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "brightnessctl set %d%% 2>/dev/null", (int)(frac * 100.0f + 0.5f));
    run_detached(cmd);
}

/* Current backlight brightness as 0..1, or -1 if none. */
static float read_brightness(void)
{
    DIR *dir = opendir("/sys/class/backlight");
    if (!dir)
        return -1.0f;
    struct dirent *ent;
    float value = -1.0f;
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
        snprintf(path, sizeof(path), "/sys/class/backlight/%.200s/max_brightness", ent->d_name);
        f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d", &max) != 1)
                max = -1;
            fclose(f);
        }
        if (cur >= 0 && max > 0) {
            value = (float)cur / (float)max;
            break;
        }
    }
    closedir(dir);
    return value;
}

/* Username + "up XhYm" subtitle (HeaderPane.qml: UserInfoService.username +
 * "up " + DgopService.uptime, falling back to "Unknown" when uptime isn't
 * available). dankc has no dgop dependency, so this reads /proc/uptime
 * directly -- mirrors the QML's *logic* (real uptime when available, else
 * "Unknown") even though the reference screenshot shows "Unknown" (the
 * user's DMS session didn't have the optional `dgop` helper installed). */
static void get_user_info(char *user, size_t user_sz, char *sub, size_t sub_sz)
{
    struct passwd *pw = getpwuid(getuid());
    const char *name = (pw && pw->pw_name && pw->pw_name[0]) ? pw->pw_name : "";
    snprintf(user, user_sz, "%s", name[0] ? name : "User");

    double up = -1.0;
    FILE *f = fopen("/proc/uptime", "r");
    if (f) {
        if (fscanf(f, "%lf", &up) != 1)
            up = -1.0;
        fclose(f);
    }
    if (up >= 0.0) {
        int total_min = (int)(up / 60.0);
        int hours = total_min / 60;
        int mins = total_min % 60;
        if (hours > 0)
            snprintf(sub, sub_sz, "up %dh %dm", hours, mins);
        else
            snprintf(sub, sub_sz, "up %dm", mins);
    } else {
        snprintf(sub, sub_sz, "Unknown");
    }
}

/* ~/.face into a nanovg image, if present. dc_render_load_icon() (render/nvg.c)
 * already wraps stb_image (PNG, and JPEG since stb_image decodes both) for
 * launcher.c's app icons, so this reuses it rather than adding a dedicated
 * image-decode dependency -- loaded/freed per-frame like launcher.c does for
 * app icons (cheap relative to the GL work already happening per frame). */
static int load_face_image(dc_render *r, int size)
{
    const char *home = getenv("HOME");
    if (!home || !home[0])
        return 0;
    char path[512];
    snprintf(path, sizeof(path), "%s/.face", home);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode))
        return 0;
    return dc_render_load_icon(r, path, size);
}

/* Truncate `buf` and append an ellipsis until it fits within `max_w` px --
 * delegates to render/shape.c's dc_shape_ellipsize() (BiDi+HarfBuzz-aware for
 * Arabic/Urdu/Hebrew device names/SSIDs, identical plain-nanovg behavior for
 * everything else) the same way bar.c's bar_ellipsize() does. No-op if `buf`
 * already fits. */
static void cc_ellipsize(dc_render *render, char *buf, size_t bufsize, float max_w)
{
    char tmp[96];
    if (bufsize > sizeof(tmp))
        bufsize = sizeof(tmp);
    dc_shape_ellipsize(render, buf, max_w, tmp, bufsize);
    memcpy(buf, tmp, bufsize);
}

/* Mirrors services/audio.c's dc_audio_read() for the default *source* (mic):
 * same wpctl tool, same "Volume: %f [MUTED]" parsing, same per-second cache.
 * Kept local (rather than extending audio.h, out of this task's touch-scope)
 * since audio.h's dc_audio_read() only ever targets @DEFAULT_AUDIO_SINK@. */
static bool audio_source_read(dc_audio_info *out)
{
    static dc_audio_info cache;
    static bool cache_ok;
    static time_t cache_time;

    time_t now = time(NULL);
    if (cache_time == now) {
        *out = cache;
        return cache_ok;
    }

    out->available = false;
    out->volume = 0;
    out->muted = false;

    FILE *pipe = popen("wpctl get-volume @DEFAULT_AUDIO_SOURCE@ 2>/dev/null", "r");
    if (!pipe)
        return false;

    char line[128];
    bool ok = false;
    if (fgets(line, sizeof(line), pipe)) {
        float volume = 0.0f;
        if (sscanf(line, "Volume: %f", &volume) == 1) {
            out->volume = (int)(volume * 100.0f + 0.5f);
            out->available = true;
            ok = true;
        }
        if (strstr(line, "MUTED"))
            out->muted = true;
    }
    pclose(pipe);

    cache = *out;
    cache_ok = ok;
    cache_time = now;
    return ok;
}

/* The default sink/source's human-readable device name (e.g. "Built-in Audio
 * Analog Stereo"), parsed from `wpctl status`'s starred Sinks:/Sources: line
 * -- audio.h has no device-name field (out of touch-scope to add one), and
 * this reuses the same wpctl binary/popen pattern as dc_audio_read() rather
 * than introducing a new IPC mechanism. Cached per-second like the reads
 * above (this can be called once per render frame during the entrance
 * animation). */
static void read_audio_device_names(char *sink_name, size_t sink_sz, char *source_name,
                                     size_t source_sz)
{
    static char cached_sink[64];
    static char cached_source[64];
    static time_t cache_time;

    time_t now = time(NULL);
    if (cache_time != now) {
        cache_time = now;
        cached_sink[0] = '\0';
        cached_source[0] = '\0';

        FILE *pipe = popen("wpctl status 2>/dev/null", "r");
        if (pipe) {
            char line[256];
            int section = 0; /* 0=other, 1=sinks, 2=sources */
            while (fgets(line, sizeof(line), pipe)) {
                if (strstr(line, "Video")) /* the Audio block always comes first */
                    break;
                if (strstr(line, "Sinks:")) {
                    section = 1;
                    continue;
                }
                if (strstr(line, "Sources:")) {
                    section = 2;
                    continue;
                }
                if (strstr(line, "Filters:") || strstr(line, "Streams:") ||
                    strstr(line, "Devices:")) {
                    section = 0;
                    continue;
                }
                if (section == 0)
                    continue;
                char *star = strchr(line, '*');
                if (!star)
                    continue;
                char *dot = strchr(star, '.');
                if (!dot)
                    continue;
                char *name = dot + 1;
                while (*name == ' ')
                    name++;
                char *bracket = strchr(name, '[');
                size_t len = bracket ? (size_t)(bracket - name) : strlen(name);
                while (len > 0 &&
                       (name[len - 1] == ' ' || name[len - 1] == '\n' || name[len - 1] == '\r'))
                    len--;
                char *dst = section == 1 ? cached_sink : cached_source;
                size_t dst_sz = section == 1 ? sizeof(cached_sink) : sizeof(cached_source);
                if (len >= dst_sz)
                    len = dst_sz - 1;
                memcpy(dst, name, len);
                dst[len] = '\0';
            }
            pclose(pipe);
        }
    }

    snprintf(sink_name, sink_sz, "%s", cached_sink);
    snprintf(source_name, source_sz, "%s", cached_source);
}

/* Media-active + expand-panel state, gathered once per render/click/motion
 * call so cc_get_layout()'s card size and the actual drawn/clickable rows
 * never disagree (same "single source of truth" discipline as cc_layout
 * itself). All three reads below are cheap, cached service calls (MPRIS: a
 * ~1/s sd-bus round trip; wifi scan: services/net.c's async fetch, drained
 * non-blocking; bluez: a cached sd-bus GetManagedObjects) -- safe to call
 * from pointer-motion, which fires every frame of a drag. */
typedef struct {
    bool media_active;
    dc_mpris_info media;

    int expand_kind; /* 0 none, 1 network, 2 bluetooth */
    int expand_rows;
    dc_net_wifi_ap net_aps[CC_MAX_EXPAND_ROWS];
    dc_bluez_device bt_devs[CC_MAX_EXPAND_ROWS];

    /* Wi-Fi known/saved networks (W2, docs/18-WIFI-BT-PLAN.md sec.2.3):
     * cross-referenced from dc_net_saved_list()/dc_net_saved_find_by_ssid()
     * against this frame's scan rows, one entry per net_aps[i] -- lets the UI
     * badge a saved SSID and forget it (Settings.Connection.Delete()) without
     * a second D-Bus round trip per click. */
    bool net_ap_saved[CC_MAX_EXPAND_ROWS];
    char net_ap_saved_path[CC_MAX_EXPAND_ROWS][64];
    bool net_ap_autoconnect[CC_MAX_EXPAND_ROWS];

    /* Bluetooth pairing (W3.1) + adapter Discoverable (W1 polish). */
    bool bt_discovering;
    bool bt_discoverable;
    dc_bluez_pair_state bt_pair_state;
    char bt_pair_mac[18];
    char bt_pair_err[96];
    dc_bluez_agent_kind bt_agent_kind;
    char bt_agent_device[64];
    char bt_agent_passkey[8];
} cc_state;

static void cc_gather_state(dc_control_center *cc, cc_state *st)
{
    memset(st, 0, sizeof(*st));
    st->media_active = dc_mpris_read(&st->media);

    if (cc->net_expanded) {
        st->expand_kind = 1;
        st->expand_rows = dc_net_wifi_scan(st->net_aps, CC_MAX_EXPAND_ROWS);

        /* Known/saved networks (W2): one dc_net_saved_list() call populates
         * the cache dc_net_saved_find_by_ssid() reads from, then a lookup per
         * visible row -- cheap enough to do every gather (same cost class as
         * dc_net_ethernet(), per net.h's doc comment). */
        dc_net_saved_net saved_buf[DC_NET_SAVED_MAX];
        dc_net_saved_list(saved_buf, DC_NET_SAVED_MAX);
        int net_rows = st->expand_rows < CC_MAX_EXPAND_ROWS ? st->expand_rows : CC_MAX_EXPAND_ROWS;
        for (int i = 0; i < net_rows; i++) {
            const dc_net_saved_net *sv = dc_net_saved_find_by_ssid(st->net_aps[i].ssid);
            if (sv) {
                st->net_ap_saved[i] = true;
                snprintf(st->net_ap_saved_path[i], sizeof(st->net_ap_saved_path[i]), "%s", sv->path);
                st->net_ap_autoconnect[i] = sv->autoconnect;
            }
        }
    } else if (cc->bt_expanded) {
        dc_bluez_info bt;
        dc_bluez_read(&bt);
        int n = bt.device_count < CC_MAX_EXPAND_ROWS ? bt.device_count : CC_MAX_EXPAND_ROWS;
        if (n > 0)
            memcpy(st->bt_devs, bt.devices, (size_t)n * sizeof(bt.devices[0]));
        st->expand_kind = 2;
        st->expand_rows = n;
        st->bt_discoverable = bt.discoverable;

        st->bt_discovering = dc_bluez_discovering();
        st->bt_pair_state = dc_bluez_pair_poll(st->bt_pair_mac, sizeof(st->bt_pair_mac),
                                               st->bt_pair_err, sizeof(st->bt_pair_err));

        dc_bluez_agent_request areq;
        if (dc_bluez_agent_poll(&areq)) {
            st->bt_agent_kind = areq.kind;
            snprintf(st->bt_agent_device, sizeof(st->bt_agent_device), "%s", areq.device_name);
            snprintf(st->bt_agent_passkey, sizeof(st->bt_agent_passkey), "%s", areq.passkey_str);
        }
    }
}

/* Builds the two per-row action-icon flag arrays cc_get_layout() needs (W1/W2
 * polish) from this frame's gathered state -- shared by every cc_get_layout()
 * call site (render/click/motion) so they always agree on which rows show a
 * Forget/Trust/Remove icon, same "one function feeds every layout consumer"
 * discipline as cc_gather_state() itself. */
static void cc_row_action_flags(const cc_state *st, bool net_saved[CC_MAX_EXPAND_ROWS],
                                bool bt_paired[CC_MAX_EXPAND_ROWS])
{
    memset(net_saved, 0, CC_MAX_EXPAND_ROWS * sizeof(bool));
    memset(bt_paired, 0, CC_MAX_EXPAND_ROWS * sizeof(bool));
    int rows = st->expand_rows > CC_MAX_EXPAND_ROWS ? CC_MAX_EXPAND_ROWS : st->expand_rows;
    if (st->expand_kind == 1) {
        for (int i = 0; i < rows; i++)
            net_saved[i] = st->net_ap_saved[i];
    } else if (st->expand_kind == 2) {
        for (int i = 0; i < rows; i++)
            bt_paired[i] = st->bt_devs[i].paired;
    }
}

/* Close the inline Wi-Fi password panel (W1.1), aborting any in-flight
 * connect job -- shared by every place that dismisses it (collapsing the
 * network section, switching to bluetooth, the Cancel button, Escape,
 * teardown). */
static void cc_close_net_pw(dc_control_center *cc)
{
    if (cc->net_pw_connecting)
        dc_net_wifi_connect_reset();
    cc->net_pw_active = false;
    cc->net_pw_connecting = false;
    cc->net_pw_buf[0] = '\0';
    cc->net_pw_err[0] = '\0';
}

/* Close the bluetooth expand section (W3.1): stops any in-flight discovery
 * and rejects/aborts any in-flight pairing job or pending agent dialog --
 * shared by every place that leaves the section (collapsing it, switching to
 * network, control-center teardown), same "closed == reset" contract as
 * cc_close_net_pw() above. */
static void cc_close_bt_section(dc_control_center *cc)
{
    if (cc->bt_expanded) {
        dc_bluez_stop_discovery();
        dc_bluez_pair_reset();
    }
    cc->bt_expanded = false;
    cc->bt_pairing_active = false;
    cc->bt_passkey_buf[0] = '\0';
}

/* 0-based row index of `cc->net_pw_ssid` within `st->net_aps`, or -1 if the
 * inline password panel isn't open or its SSID has scrolled out of the
 * latest scan (a fresh nmcli scan can legitimately drop a weak/out-of-range
 * AP -- the panel simply won't have anywhere to attach until it reappears,
 * same tolerance the old "Needs Password" hint had for a stale SSID). Shared
 * by cc_render()/handle_click()/handle_motion() so all three agree on where
 * (or whether) the panel is drawn this frame -- same "one function drives
 * measure+draw" discipline as cc_hittest(). */
static int cc_find_pw_row(const dc_control_center *cc, const cc_state *st)
{
    if (!cc->net_pw_active || st->expand_kind != 1)
        return -1;
    int rows = st->expand_rows < CC_MAX_EXPAND_ROWS ? st->expand_rows : CC_MAX_EXPAND_ROWS;
    for (int i = 0; i < rows; i++) {
        if (strcmp(st->net_aps[i].ssid, cc->net_pw_ssid) == 0)
            return i;
    }
    return -1;
}

/* CompoundPill-style tile (Widgets/CompoundPill.qml): pill background stays
 * constant, only the icon chip fills solid-primary when active; two stacked
 * text lines (title/subtitle) to the right. Used for wifi/bluetooth/
 * audioOutput/audioInput. */
static void draw_pill_tile(dc_render *r, float x, float y, float w, float h, int icon,
                           const char *title, const char *subtitle, bool active)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);

    const float chip_pad = 8.0f;
    const float chip = h - 2.0f * chip_pad;
    const float chip_x = x + chip_pad;
    const float chip_y = y + chip_pad;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, chip_x, chip_y, chip, chip, 10.0f);
    nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_container_highest));
    nvgFill(vg);
    dc_render_icon(r, icon, chip_x + chip / 2.0f, chip_y + chip / 2.0f, 20.0f,
                   active ? t->primary_text : t->primary, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    const float text_x = chip_x + chip + 12.0f;
    const float text_w = (x + w - 12.0f) - text_x;

    char title_buf[64];
    snprintf(title_buf, sizeof(title_buf), "%s", title);
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 14.0f);
    cc_ellipsize(r, title_buf, sizeof(title_buf), text_w);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    dc_shape_draw_text(r, text_x, y + h / 2.0f - 9.0f, title_buf, NULL);

    if (subtitle && subtitle[0]) {
        char sub_buf[64];
        snprintf(sub_buf, sizeof(sub_buf), "%s", subtitle);
        nvgFontSize(vg, DC_BAR_TEXT_SIZE);
        cc_ellipsize(r, sub_buf, sizeof(sub_buf), text_w);
        nvgFillColor(vg, tc(t->surface_variant_text));
        dc_shape_draw_text(r, text_x, y + h / 2.0f + 9.0f, sub_buf, NULL);
    }
}

/* ToggleButton-style tile (Widgets/ToggleButton.qml): the whole tile fills
 * solid-primary when active (dark text/icon on the light-green fill), a
 * single icon+label line when inactive. NOTE the inactive icon and label use
 * *different* colors (Theme.ccTileInactiveIcon=primary vs
 * Theme.surfaceText) -- matches the QML exactly, not a typo. Used for
 * nightMode/darkMode. */
static void draw_toggle_tile(dc_render *r, float x, float y, float w, float h, int icon,
                             const char *label, bool active)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, x, y, w, h, 12.0f);
    nvgFillColor(vg, active ? tc(t->primary) : tc(t->surface_container_high));
    nvgFill(vg);

    dc_color icon_fg = active ? t->primary_text : t->primary;
    dc_color text_fg = active ? t->primary_text : t->surface_text;

    dc_render_icon(r, icon, x + 18.0f, y + h / 2.0f, 20.0f, icon_fg,
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 14.0f);
    nvgFillColor(vg, tc(text_fg));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + 48.0f, y + h / 2.0f, label, NULL);
}

/* A horizontal slider: icon + rounded track + primary fill + a live "NN%"
 * label on the trailing edge (docs/13-POPOUTS-SPEC.md sec.1: "green fill,
 * rounded, ~12px tall track" + item 3 numeric value). */
static void draw_slider(dc_render *r, float x, float cy, float w, int icon, float value)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;
    if (value < 0.0f)
        value = 0.0f;
    if (value > 1.0f)
        value = 1.0f;

    dc_render_icon(r, icon, x, cy, 20.0f, t->surface_text, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    const float tx = x + 32.0f;
    const float tw = w - 32.0f - CC_SLIDER_LABEL_W;
    const float th = 12.0f;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, tx, cy - th / 2.0f, tw, th, th / 2.0f);
    nvgFillColor(vg, tc_alpha(t->outline, 90));
    nvgFill(vg);

    float fw = tw * value;
    if (fw < th)
        fw = th;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, tx, cy - th / 2.0f, fw, th, th / 2.0f);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);

    char pct[8];
    snprintf(pct, sizeof(pct), "%d%%", (int)(value * 100.0f + 0.5f));
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
    nvgText(vg, x + w, cy, pct, NULL);
}

/* Which pairing-agent request kinds (W3.1) need a typed-entry field in
 * draw_bt_agent_panel(), as opposed to just Reject/Confirm buttons:
 * RequestPasskey (digits only) and RequestPinCode (legacy pre-SSP pairing,
 * any printable char, up to 16 per the Bluetooth spec) -- both answerable via
 * a typed value, unlike RequestConfirmation/RequestAuthorization's plain
 * yes/no. Shared by cc_get_layout()/cc_hittest()/draw_bt_agent_panel()/the
 * click and key handlers so none of them can disagree about which kind gets
 * the field. */
static bool cc_bt_agent_has_field(dc_bluez_agent_kind k)
{
    return k == DC_BLUEZ_AGENT_PASSKEY || k == DC_BLUEZ_AGENT_PIN_CODE;
}

/* Map org.bluez.Device1.Icon (dc_bluez_device.icon) onto dankc's own icon set
 * (docs/18-WIFI-BT-PLAN.md W1 item 5) -- audio-headphones/audio-headset ->
 * headphones, input-mouse -> mouse, input-keyboard -> keyboard, phone ->
 * smartphone, else a generic bluetooth glyph (connected/plain per the
 * existing connection-state cue) so a still-unpaired nearby device without a
 * reported type keeps its "tap to pair" affordance. */
static int cc_bt_device_icon(const dc_bluez_device *d)
{
    const char *ic = d->icon;
    if (ic[0]) {
        if (strstr(ic, "headphone") || strstr(ic, "headset"))
            return DC_ICON_HEADPHONES;
        if (strstr(ic, "mouse"))
            return DC_ICON_MOUSE;
        if (strstr(ic, "keyboard"))
            return DC_ICON_KEYBOARD;
        if (strstr(ic, "phone"))
            return DC_ICON_SMARTPHONE;
    }
    if (!d->paired)
        return DC_ICON_ADD; /* nearby, unpaired, unknown type -- "tap to pair" */
    return d->connected ? DC_ICON_BLUETOOTH_CONNECTED : DC_ICON_BLUETOOTH;
}

/* Extra trailing width draw_expand_row() must leave clear of its own text so
 * the per-row action icons (drawn separately by the caller, at cc_layout's
 * fixed net_forget_cx/bt_trust_cx/bt_remove_cx positions -- see cc_render())
 * never overlap the title/status text. A little more than the icons' own
 * footprint (2*r, or 4*r plus the gap between two) so there's visible
 * breathing room. */
#define CC_ROW_FORGET_RESERVE_W 28.0f
#define CC_ROW_BT_ACTIONS_RESERVE_W 52.0f

/* One row of the network/bluetooth expand panel: small leading icon, title
 * (ellipsized), an optional small "extra" label (Bluetooth battery %,
 * right-aligned just left of `status`), an optional small badge icon (Wi-Fi
 * "saved network" pin, between the title and status/extra), trailing status
 * text (colored `status_primary` when it should stand out -- "Connected"/
 * in-use, or a warning tone for the password hint), and `trailing_reserve_w`
 * px of clear space left of all of the above for the caller's own per-row
 * action icons (see the #defines above). Deliberately flatter than
 * draw_pill_tile() (no icon chip/background) since these are dense list rows,
 * not tiles. */
static void draw_expand_row(dc_render *r, float x, float y, float w, float h, int icon,
                            const char *title, const char *status, bool status_primary,
                            bool status_warn, const char *extra_label, int badge_icon,
                            float trailing_reserve_w)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    dc_render_icon(r, icon, x + 12.0f, y + h / 2.0f, 15.0f,
                   status_primary ? t->primary : t->surface_variant_text,
                   NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);

    char status_buf[32];
    snprintf(status_buf, sizeof(status_buf), "%s", status ? status : "");
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 12.0f);
    float sbounds[4];
    nvgTextBounds(vg, 0, 0, status_buf, NULL, sbounds);
    float status_w = status_buf[0] ? (sbounds[2] - sbounds[0]) + 8.0f : 0.0f;

    char extra_buf[16];
    snprintf(extra_buf, sizeof(extra_buf), "%s", extra_label ? extra_label : "");
    float extra_w = 0.0f;
    if (extra_buf[0]) {
        nvgTextBounds(vg, 0, 0, extra_buf, NULL, sbounds);
        extra_w = (sbounds[2] - sbounds[0]) + 10.0f;
    }
    float badge_w = badge_icon != 0 ? 16.0f : 0.0f;

    char title_buf[96];
    snprintf(title_buf, sizeof(title_buf), "%s", title ? title : "");
    nvgFontSize(vg, 13.0f);
    cc_ellipsize(r, title_buf, sizeof(title_buf),
                w - 30.0f - status_w - extra_w - badge_w - trailing_reserve_w);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    dc_shape_draw_text(r, x + 30.0f, y + h / 2.0f, title_buf, NULL);

    float cursor = x + w - 4.0f - trailing_reserve_w;
    if (status_buf[0]) {
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, tc(status_warn ? t->warning : (status_primary ? t->primary : t->surface_variant_text)));
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg, cursor, y + h / 2.0f, status_buf, NULL);
        cursor -= status_w;
    }
    if (extra_buf[0]) {
        nvgFontSize(vg, 11.0f);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgTextAlign(vg, NVG_ALIGN_RIGHT | NVG_ALIGN_MIDDLE);
        nvgText(vg, cursor, y + h / 2.0f, extra_buf, NULL);
        cursor -= extra_w;
    }
    if (badge_icon != 0)
        dc_render_icon(r, badge_icon, cursor - 6.0f, y + h / 2.0f, 12.0f, t->surface_variant_text,
                      NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

/* Inline Wi-Fi password entry (W1.1): a masked text field (dot per
 * character, matching DMS's WifiPasswordModal.qml `echoMode:
 * TextInput.Password`) + a status slot (blank / "Connecting..." / an inline
 * error) + Cancel/Connect buttons, drawn directly below the SSID row it
 * belongs to (see cc_get_layout()'s pw_after_row geometry). Layout (box
 * positions/sizes) is entirely owned by cc_get_layout()/cc_hittest() so this
 * function only ever reads `l`, never computes its own coordinates -- same
 * discipline as every other draw_*() helper in this file. */
static void draw_net_pw_panel(dc_render *r, const cc_layout *l, const char *pw_buf,
                              bool connecting, const char *err_msg)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    /* Password field: dot-masked, matches ui_textfield()'s box style
     * (settings.c) -- rounded rect + a 2px primary border since this is the
     * only text field the control center ever shows (no separate "focused"
     * state needed). */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l->ix, l->pw_field_y, l->iw, l->pw_field_h, 8.0f);
    nvgFillColor(vg, tc(t->surface_container_highest));
    nvgFill(vg);
    nvgStrokeWidth(vg, 2.0f);
    nvgStrokeColor(vg, tc(t->primary));
    nvgStroke(vg);

    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 13.0f);
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    size_t pw_len = pw_buf ? strlen(pw_buf) : 0;
    if (pw_len == 0) {
        nvgFillColor(vg, tc_alpha(t->surface_variant_text, 170));
        nvgText(vg, l->ix + 10.0f, l->pw_field_y + l->pw_field_h / 2.0f, "Password", NULL);
    } else {
        /* Each bullet is the 3-byte UTF-8 sequence for U+2022, one per
         * password character (capped so the buffer can't overflow even at
         * CC_NET_PW_MAX length). */
        char masked[3 * 64 + 1];
        size_t shown = pw_len < 64 ? pw_len : 64;
        for (size_t i = 0; i < shown; i++)
            memcpy(masked + i * 3, "\xe2\x80\xa2", 3);
        masked[shown * 3] = '\0';
        nvgFillColor(vg, tc(t->surface_text));
        nvgText(vg, l->ix + 10.0f, l->pw_field_y + l->pw_field_h / 2.0f, masked, NULL);
    }

    /* Status slot: blank, "Connecting...", or the last inline error. */
    if (connecting) {
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, l->ix, l->pw_status_y, "Connecting\xe2\x80\xa6", NULL);
    } else if (err_msg && err_msg[0]) {
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, tc(t->warning));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        char buf[96];
        snprintf(buf, sizeof(buf), "%s", err_msg);
        nvgText(vg, l->ix, l->pw_status_y, buf, NULL);
    }

    /* Cancel button (outline). */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l->pw_cancel_x0, l->pw_btn_y0, l->pw_cancel_x1 - l->pw_cancel_x0,
                  l->pw_btn_h, l->pw_btn_h / 2.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(vg, (l->pw_cancel_x0 + l->pw_cancel_x1) / 2.0f, l->pw_btn_y0 + l->pw_btn_h / 2.0f,
           "Cancel", NULL);

    /* Connect button (filled primary; dimmed while connecting/empty -- pure
     * visual feedback, the click handler already ignores both cases). */
    bool disabled = connecting || pw_len == 0;
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l->pw_connect_x0, l->pw_btn_y0, l->pw_connect_x1 - l->pw_connect_x0,
                  l->pw_btn_h, l->pw_btn_h / 2.0f);
    nvgFillColor(vg, disabled ? tc_alpha(t->primary, 130) : tc(t->primary));
    nvgFill(vg);
    nvgFillColor(vg, tc(t->primary_text));
    nvgText(vg, (l->pw_connect_x0 + l->pw_connect_x1) / 2.0f, l->pw_btn_y0 + l->pw_btn_h / 2.0f,
           connecting ? "Connecting\xe2\x80\xa6" : "Connect", NULL);
}

/* Bluetooth pairing-agent panel (W3.1): a one-line prompt ("Confirm code
 * NNNNNN with \"device\"?" / "Pair with \"device\"?" / "Enter the passkey
 * shown on \"device\"" / "Enter the PIN for \"device\"") plus either a masked
 * entry field (RequestPasskey: digits only; RequestPinCode: any printable
 * char, legacy pre-SSP pairing -- see cc_bt_agent_has_field()) or nothing
 * extra, then Reject/Confirm (or Cancel/Submit) buttons -- same "layout owns
 * geometry, this only draws" discipline as draw_net_pw_panel() above, whose
 * masked-field styling this reuses for both entry-field cases. */
static void draw_bt_agent_panel(dc_render *r, const cc_layout *l, const char *device_name,
                                const char *passkey_str, const char *entry_buf)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    char text[128];
    if (l->bt_agent_kind == DC_BLUEZ_AGENT_CONFIRM)
        snprintf(text, sizeof(text), "Confirm code %s with \"%s\"?",
                 passkey_str ? passkey_str : "", device_name ? device_name : "device");
    else if (l->bt_agent_kind == DC_BLUEZ_AGENT_PASSKEY)
        snprintf(text, sizeof(text), "Enter the passkey shown on \"%s\"",
                 device_name ? device_name : "device");
    else if (l->bt_agent_kind == DC_BLUEZ_AGENT_PIN_CODE)
        snprintf(text, sizeof(text), "Enter the PIN for \"%s\"", device_name ? device_name : "device");
    else
        snprintf(text, sizeof(text), "Pair with \"%s\"?", device_name ? device_name : "device");

    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 12.5f);
    cc_ellipsize(r, text, sizeof(text), l->iw);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    dc_shape_draw_text(r, l->ix, l->bt_agent_text_y, text, NULL);

    size_t entry_len = entry_buf ? strlen(entry_buf) : 0;
    bool has_field = cc_bt_agent_has_field(l->bt_agent_kind);
    const char *placeholder = l->bt_agent_kind == DC_BLUEZ_AGENT_PIN_CODE ? "PIN" : "Passkey";

    if (has_field) {
        nvgBeginPath(vg);
        nvgRoundedRect(vg, l->ix, l->bt_agent_field_y, l->iw, l->bt_agent_field_h, 8.0f);
        nvgFillColor(vg, tc(t->surface_container_highest));
        nvgFill(vg);
        nvgStrokeWidth(vg, 2.0f);
        nvgStrokeColor(vg, tc(t->primary));
        nvgStroke(vg);

        nvgFontSize(vg, 13.0f);
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        if (entry_len == 0) {
            nvgFillColor(vg, tc_alpha(t->surface_variant_text, 170));
            nvgText(vg, l->ix + 10.0f, l->bt_agent_field_y + l->bt_agent_field_h / 2.0f, placeholder,
                   NULL);
        } else {
            char masked[3 * 16 + 1];
            size_t shown = entry_len < 16 ? entry_len : 16;
            for (size_t i = 0; i < shown; i++)
                memcpy(masked + i * 3, "\xe2\x80\xa2", 3);
            masked[shown * 3] = '\0';
            nvgFillColor(vg, tc(t->surface_text));
            nvgText(vg, l->ix + 10.0f, l->bt_agent_field_y + l->bt_agent_field_h / 2.0f, masked,
                   NULL);
        }
    }

    const char *no_label = has_field ? "Cancel" : "Reject";
    const char *yes_label = has_field ? "Submit" : "Confirm";
    bool yes_disabled = has_field && entry_len == 0;

    nvgBeginPath(vg);
    nvgRoundedRect(vg, l->bt_agent_no_x0, l->bt_agent_btn_y0, l->bt_agent_no_x1 - l->bt_agent_no_x0,
                  l->bt_agent_btn_h, l->bt_agent_btn_h / 2.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    nvgFontSize(vg, 12.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    nvgText(vg, (l->bt_agent_no_x0 + l->bt_agent_no_x1) / 2.0f,
           l->bt_agent_btn_y0 + l->bt_agent_btn_h / 2.0f, no_label, NULL);

    nvgBeginPath(vg);
    nvgRoundedRect(vg, l->bt_agent_yes_x0, l->bt_agent_btn_y0,
                  l->bt_agent_yes_x1 - l->bt_agent_yes_x0, l->bt_agent_btn_h,
                  l->bt_agent_btn_h / 2.0f);
    nvgFillColor(vg, yes_disabled ? tc_alpha(t->primary, 130) : tc(t->primary));
    nvgFill(vg);
    nvgFillColor(vg, tc(t->primary_text));
    nvgText(vg, (l->bt_agent_yes_x0 + l->bt_agent_yes_x1) / 2.0f,
           l->bt_agent_btn_y0 + l->bt_agent_btn_h / 2.0f, yes_label, NULL);
}

/* Media transport row (docs/13-POPOUTS-SPEC.md sec.1 item 4): art-circle
 * placeholder + title/artist + prev/play-pause/next, shown only while an
 * MPRIS player is active. Mirrors the bar's media widget colors (title
 * normal text, artist in the primary/green accent per the dashboard Media
 * tab spec) rather than pulling in album art decoding (out of scope here --
 * that's the Dashboard Media tab's job, docs/13-POPOUTS-SPEC.md sec.5). */
static void draw_media_row(dc_render *r, const cc_layout *l, const dc_mpris_info *mp)
{
    NVGcontext *vg = r->vg;
    const dc_theme *t = dc_theme_current;

    nvgBeginPath(vg);
    nvgCircle(vg, l->media_art_cx, l->media_art_cy, l->media_art_r);
    nvgFillColor(vg, tc(t->surface_container_highest));
    nvgFill(vg);
    dc_render_icon(r, DC_ICON_MUSIC_NOTE, l->media_art_cx, l->media_art_cy, 18.0f, t->primary,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char title[sizeof(mp->title)];
    snprintf(title, sizeof(title), "%s", mp->title[0] ? mp->title : "Not playing");
    nvgFontFaceId(vg, r->font_ui);
    nvgFontSize(vg, 13.0f);
    cc_ellipsize(r, title, sizeof(title), l->media_text_w);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    dc_shape_draw_text(r, l->media_text_x, l->media_art_cy - 9.0f, title, NULL);

    if (mp->artist[0]) {
        char artist[sizeof(mp->artist)];
        snprintf(artist, sizeof(artist), "%s", mp->artist);
        nvgFontSize(vg, DC_BAR_TEXT_SIZE);
        cc_ellipsize(r, artist, sizeof(artist), l->media_text_w);
        nvgFillColor(vg, tc(t->primary));
        dc_shape_draw_text(r, l->media_text_x, l->media_art_cy + 9.0f, artist, NULL);
    }

    dc_render_icon(r, DC_ICON_SKIP_PREVIOUS, l->media_btn_cx[0], l->media_btn_cy, 16.0f,
                   t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    nvgBeginPath(vg);
    nvgCircle(vg, l->media_btn_cx[1], l->media_btn_cy, l->media_btn_r);
    nvgFillColor(vg, tc(t->primary));
    nvgFill(vg);
    dc_render_icon(r, mp->playing ? DC_ICON_PAUSE : DC_ICON_PLAY_ARROW, l->media_btn_cx[1],
                   l->media_btn_cy, 16.0f, t->primary_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    dc_render_icon(r, DC_ICON_SKIP_NEXT, l->media_btn_cx[2], l->media_btn_cy, 16.0f,
                   t->surface_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
}

static void recompute_physical(dc_control_center *cc)
{
    cc->phys_width = (cc->logical_width * cc->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
    cc->phys_height = (cc->logical_height * cc->scale120 + DC_SCALE_BASE / 2) / DC_SCALE_BASE;
}

/* Hover bg (docs/13-POPOUTS-SPEC.md sec.1; formula from bar.c's
 * draw_hover_overlay()): painted last, on top of whatever's already drawn at
 * that hit rect, so callers never need their own hover-aware branch. Circles
 * for the header buttons, rounded rects for the slider row / tiles. */
static void draw_cc_hover(dc_control_center *cc, const cc_layout *l)
{
    if (cc->hover_id == CC_HOVER_NONE)
        return;

    NVGcontext *vg = cc->render->vg;
    const dc_theme *t = dc_theme_current;
    const dc_config *cfg = dc_config_current;
    dc_color hc =
        dc_hover_bg_color(t->surface_container_high, t->primary, cfg->bar_widget_transparency);
    NVGcolor col = nvgRGBA(hc.r, hc.g, hc.b, hc.a);

    if (cc->hover_id >= CC_HOVER_BTN_LOCK && cc->hover_id <= CC_HOVER_BTN_EDIT) {
        int i = cc->hover_id - CC_HOVER_BTN_LOCK;
        nvgBeginPath(vg);
        nvgCircle(vg, l->btn_cx[i], l->btn_cy, l->btn_r);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id == CC_HOVER_SLIDER_VOLUME || cc->hover_id == CC_HOVER_SLIDER_BRIGHTNESS) {
        int i = cc->hover_id - CC_HOVER_SLIDER_VOLUME;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, l->slot_x[i], l->sliders_y, l->slot_w, l->slider_h, 10.0f);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id >= CC_HOVER_TILE_0_0 && cc->hover_id <= CC_HOVER_TILE_2_1) {
        int idx = cc->hover_id - CC_HOVER_TILE_0_0;
        int row = idx / 2, col_i = idx % 2;
        float rx = cc_tile_x(l, col_i), ry = cc_tile_y(l, row);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, rx, ry, l->tile_w, l->tile_h, 12.0f);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id == CC_HOVER_WIFI_CHEVRON || cc->hover_id == CC_HOVER_BT_CHEVRON) {
        float cx = cc->hover_id == CC_HOVER_WIFI_CHEVRON ? l->wifi_chevron_cx : l->bt_chevron_cx;
        float cy = cc->hover_id == CC_HOVER_WIFI_CHEVRON ? l->wifi_chevron_cy : l->bt_chevron_cy;
        nvgBeginPath(vg);
        nvgCircle(vg, cx, cy, l->chevron_r);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id >= CC_HOVER_MEDIA_PREV && cc->hover_id <= CC_HOVER_MEDIA_NEXT) {
        int i = cc->hover_id - CC_HOVER_MEDIA_PREV;
        nvgBeginPath(vg);
        nvgCircle(vg, l->media_btn_cx[i], l->media_btn_cy, l->media_btn_r);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id >= CC_HOVER_EXPAND_ROW_BASE && cc->hover_id < CC_HOVER_NET_PW_FIELD) {
        int i = cc->hover_id - CC_HOVER_EXPAND_ROW_BASE;
        float ry = cc_expand_row_y(l, i);
        nvgBeginPath(vg);
        nvgRoundedRect(vg, l->ix, ry, l->iw, l->expand_row_h, 8.0f);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id == CC_HOVER_NET_PW_CANCEL || cc->hover_id == CC_HOVER_NET_PW_CONNECT) {
        bool is_cancel = cc->hover_id == CC_HOVER_NET_PW_CANCEL;
        float bx0 = is_cancel ? l->pw_cancel_x0 : l->pw_connect_x0;
        float bx1 = is_cancel ? l->pw_cancel_x1 : l->pw_connect_x1;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx0, l->pw_btn_y0, bx1 - bx0, l->pw_btn_h, l->pw_btn_h / 2.0f);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id == CC_HOVER_BT_SCAN_BTN || cc->hover_id == CC_HOVER_BT_DISCOVERABLE_BTN) {
        bool is_scan = cc->hover_id == CC_HOVER_BT_SCAN_BTN;
        float bx0 = is_scan ? l->bt_scan_btn_x0 : l->bt_disc_btn_x0;
        float bx1 = is_scan ? l->bt_scan_btn_x1 : l->bt_disc_btn_x1;
        float by0 = is_scan ? l->bt_scan_btn_y0 : l->bt_disc_btn_y0;
        float bh = is_scan ? l->bt_scan_btn_h : l->bt_disc_btn_h;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx0, by0, bx1 - bx0, bh, bh / 2.0f);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id == CC_HOVER_BT_AGENT_NO || cc->hover_id == CC_HOVER_BT_AGENT_YES) {
        bool is_no = cc->hover_id == CC_HOVER_BT_AGENT_NO;
        float bx0 = is_no ? l->bt_agent_no_x0 : l->bt_agent_yes_x0;
        float bx1 = is_no ? l->bt_agent_no_x1 : l->bt_agent_yes_x1;
        nvgBeginPath(vg);
        nvgRoundedRect(vg, bx0, l->bt_agent_btn_y0, bx1 - bx0, l->bt_agent_btn_h,
                      l->bt_agent_btn_h / 2.0f);
        nvgFillColor(vg, col);
        nvgFill(vg);
        return;
    }

    if (cc->hover_id >= CC_HOVER_BT_TRUST_BASE && cc->hover_id < CC_HOVER_NET_FORGET_BASE + CC_MAX_EXPAND_ROWS) {
        int idx;
        float cx;
        if (cc->hover_id < CC_HOVER_BT_REMOVE_BASE) {
            idx = cc->hover_id - CC_HOVER_BT_TRUST_BASE;
            cx = l->bt_trust_cx;
        } else if (cc->hover_id < CC_HOVER_NET_FORGET_BASE) {
            idx = cc->hover_id - CC_HOVER_BT_REMOVE_BASE;
            cx = l->bt_remove_cx;
        } else {
            idx = cc->hover_id - CC_HOVER_NET_FORGET_BASE;
            cx = l->net_forget_cx;
        }
        float ry = cc_expand_row_y(l, idx);
        nvgBeginPath(vg);
        nvgCircle(vg, cx, ry + l->expand_row_h / 2.0f, l->bt_action_r);
        nvgFillColor(vg, col);
        nvgFill(vg);
    }
}

static void cc_render(dc_control_center *cc)
{
    if (!cc->configured || cc->phys_width <= 0)
        return;

    /* Gather media/expand state and compute the layout *before* touching the
     * EGL window, since an expanded network/bluetooth section or an active
     * media player changes the card's total height -- cc_get_layout()'s
     * total_h is this frame's authoritative height, and the layer-surface is
     * resized (protocol-level) to match right here so draw + hit-test + the
     * actual mapped surface size never disagree (docs/13-POPOUTS-SPEC.md
     * sec.1 items 1/2/4). */
    cc_state st;
    cc_gather_state(cc, &st);

    /* Progress the password-entry connect job (W1.1), if any, before this
     * frame's layout so a just-resolved success/failure is reflected right
     * away instead of flashing one extra "Connecting..." frame. */
    if (cc->net_pw_connecting) {
        char err[sizeof(cc->net_pw_err)] = "";
        dc_net_connect_state cstate = dc_net_wifi_connect_poll(err, sizeof(err));
        if (cstate == DC_NET_CONNECT_SUCCESS) {
            cc->net_pw_active = false;
            cc->net_pw_connecting = false;
            cc->net_pw_buf[0] = '\0';
            cc->net_pw_err[0] = '\0';
            dc_net_wifi_connect_reset();
        } else if (cstate == DC_NET_CONNECT_FAILED) {
            cc->net_pw_connecting = false;
            snprintf(cc->net_pw_err, sizeof(cc->net_pw_err), "%s",
                    err[0] ? err : "Connection failed");
            dc_net_wifi_connect_reset();
        }
        /* DC_NET_CONNECT_IN_PROGRESS: nothing to do yet -- still showing
         * "Connecting..."; the frame_cb request below keeps polling. */
    }

    /* Bluetooth pairing job (W3.1): cc_gather_state() above already polled
     * dc_bluez_pair_poll() into st.bt_pair_state whenever the bluetooth
     * section is open -- once it's reached a terminal state, stop requesting
     * extra frame callbacks purely to watch it (the resolved status stays
     * visible on the device's row regardless, via bluez.c's own job state,
     * until the next pair attempt or section close). */
    if (cc->bt_pairing_active && cc->bt_expanded &&
        (st.bt_pair_state == DC_BLUEZ_PAIR_SUCCESS || st.bt_pair_state == DC_BLUEZ_PAIR_FAILED))
        cc->bt_pairing_active = false;

    bool net_row_saved[CC_MAX_EXPAND_ROWS], bt_row_paired[CC_MAX_EXPAND_ROWS];
    cc_row_action_flags(&st, net_row_saved, bt_row_paired);
    cc_layout l = cc_get_layout((float)cc->logical_width, st.media_active, st.expand_kind,
                                st.expand_rows, cc_find_pw_row(cc, &st), st.bt_agent_kind,
                                net_row_saved, bt_row_paired);

    dc_info("TEMPDBG cc_render: logical_width=%d net_expanded=%d bt_expanded=%d expand_kind=%d "
           "expand_rows=%d total_h=%f", cc->logical_width, cc->net_expanded, cc->bt_expanded,
           l.expand_kind, l.expand_rows, (double)l.total_h);
    int desired_h = (int)ceilf(l.total_h);
    if (desired_h != cc->logical_height && cc->layer_surface) {
        cc->logical_height = desired_h;
        recompute_physical(cc);
        zwlr_layer_surface_v1_set_size(cc->layer_surface, (uint32_t)DC_CC_WIDTH,
                                       (uint32_t)desired_h);
        /* Applies the pending set_size (double-buffered, like every other
         * layer-shell request) without waiting for the compositor's
         * configure ack -- same "optimistic" client-side sizing the initial
         * cc_show()/layer_surface_handle_configure() path already relies on. */
        wl_surface_commit(cc->surface);
    }

    if (!cc->egl_ready) {
        if (!dc_egl_window_init(&cc->egl_window, cc->egl, cc->surface, cc->phys_width,
                                cc->phys_height))
            return;
        cc->egl_ready = true;
    } else {
        dc_egl_window_resize(&cc->egl_window, cc->phys_width, cc->phys_height);
    }

    if (!dc_egl_make_current(cc->egl, &cc->egl_window))
        return;
    if (!dc_render_ensure(cc->render))
        return;

    if (cc->viewport)
        wp_viewport_set_destination(cc->viewport, cc->logical_width, cc->logical_height);

    NVGcontext *vg = cc->render->vg;
    const dc_theme *t = dc_theme_current;
    const float w = cc->logical_width;
    const float h = cc->logical_height;
    const float pad = 6.0f; /* room for the drop shadow */

    glViewport(0, 0, cc->phys_width, cc->phys_height);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f); /* transparent -> rounded card over wallpaper */
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    nvgBeginFrame(vg, w, h, (float)cc->scale120 / DC_SCALE_BASE);

    /* Entrance/exit: fade + scale from the bar-facing edge (docs/13-POPOUTS-
     * SPEC.md sec.0) — pivot set by dc_popout_bar_adjacent() in cc_show().
     * Closing runs the progress in reverse. */
    float p = dc_anim_progress(&cc->anim);
    if (cc->closing)
        p = 1.0f - (p > 1.0f ? 1.0f : p);
    float alpha = p > 1.0f ? 1.0f : p;
    float scale = 0.94f + 0.06f * p;
    float ox = pad + (w - 2.0f * pad) * cc->anim_ox;
    float oy = pad + (h - 2.0f * pad) * cc->anim_oy;
    nvgGlobalAlpha(vg, alpha);
    nvgTranslate(vg, ox, oy);
    nvgScale(vg, scale, scale);
    nvgTranslate(vg, -ox, -oy);

    /* Soft drop shadow. */
    NVGpaint shadow = nvgBoxGradient(vg, pad, pad + 2.0f, w - 2 * pad, h - 2 * pad, 12.0f, 18.0f,
                                     nvgRGBA(0, 0, 0, 90), nvgRGBA(0, 0, 0, 0));
    nvgBeginPath(vg);
    nvgRect(vg, 0, 0, w, h);
    nvgRoundedRect(vg, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgPathWinding(vg, NVG_HOLE);
    nvgFillPaint(vg, shadow);
    nvgFill(vg);

    /* Card: blurred+dimmed wallpaper ("material" bg) when enabled, else the
     * flat surfaceContainer fill (docs/POLISH.md P2, ui/material_bg.c). No
     * separate "Control Center" title -- the reference screenshot goes
     * straight from the card edge into the user header card
     * (docs/13-POPOUTS-SPEC.md sec.1). */
    dc_material_bg_fill_card(vg, cc->render, pad, pad, w - 2 * pad, h - 2 * pad, 12.0f);
    nvgStrokeColor(vg, nvgRGBA(t->outline.r, t->outline.g, t->outline.b, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    /* --- User header card (HeaderPane.qml) --------------------------- */
    nvgBeginPath(vg);
    nvgRoundedRect(vg, l.ix, l.header_y, l.iw, l.header_h, 12.0f);
    nvgFillColor(vg, tc(t->surface_container_high));
    nvgFill(vg);
    nvgStrokeColor(vg, tc_alpha(t->outline, 40));
    nvgStrokeWidth(vg, 1.0f);
    nvgStroke(vg);

    char username[64], subtitle[64];
    get_user_info(username, sizeof(username), subtitle, sizeof(subtitle));

    /* Avatar: ~/.face if present (loaded via the same PNG/JPEG decoder
     * launcher.c already uses for app icons); otherwise a letter-avatar
     * circle (first letter of the username), per this task's explicit
     * fallback spec rather than DMS's generic "person" glyph fallback. */
    int face_img = load_face_image(cc->render, 60);
    if (face_img > 0) {
        NVGpaint pat = nvgImagePattern(vg, l.avatar_cx - l.avatar_r, l.avatar_cy - l.avatar_r,
                                       l.avatar_r * 2.0f, l.avatar_r * 2.0f, 0.0f, face_img, 1.0f);
        nvgBeginPath(vg);
        nvgCircle(vg, l.avatar_cx, l.avatar_cy, l.avatar_r);
        nvgFillPaint(vg, pat);
        nvgFill(vg);
        nvgDeleteImage(vg, face_img);
    } else {
        nvgBeginPath(vg);
        nvgCircle(vg, l.avatar_cx, l.avatar_cy, l.avatar_r);
        nvgFillColor(vg, tc(t->surface_container_highest));
        nvgFill(vg);
        if (username[0]) {
            char initial[2] = {(char)toupper((unsigned char)username[0]), '\0'};
            nvgFontFaceId(vg, cc->render->font_ui);
            nvgFontSize(vg, 22.0f);
            nvgFillColor(vg, tc(t->surface_text));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, l.avatar_cx, l.avatar_cy, initial, NULL);
        } else {
            dc_render_icon(cc->render, DC_ICON_PERSON, l.avatar_cx, l.avatar_cy, 26.0f,
                           t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
        }
    }

    const float text_x = l.avatar_cx + l.avatar_r + 12.0f;
    nvgFontFaceId(vg, cc->render->font_ui);
    nvgFontSize(vg, 15.0f);
    nvgFillColor(vg, tc(t->surface_text));
    nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
    nvgText(vg, text_x, l.avatar_cy - 9.0f, username, NULL);
    nvgFontSize(vg, DC_BAR_TEXT_SIZE);
    nvgFillColor(vg, tc(t->surface_variant_text));
    nvgText(vg, text_x, l.avatar_cy + 9.0f, subtitle, NULL);

    /* lock / power / settings / edit (docs/13-POPOUTS-SPEC.md sec.1). Hover
     * tint is painted by draw_cc_hover() below (on top of everything, same
     * convention as bar.c's draw_hover_overlay()), so the icons themselves
     * never need to know about hover state. */
    dc_render_icon(cc->render, DC_ICON_LOCK, l.btn_cx[0], l.btn_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    dc_render_icon(cc->render, DC_ICON_POWER, l.btn_cx[1], l.btn_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    dc_render_icon(cc->render, DC_ICON_SETTINGS, l.btn_cx[2], l.btn_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
    dc_render_icon(cc->render, DC_ICON_EDIT, l.btn_cx[3], l.btn_cy, 18.0f, t->surface_text,
                   NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    /* --- Live state for the sliders + tile grid ----------------------- */
    dc_audio_info audio_out;
    bool have_out = dc_audio_read(&audio_out);
    dc_audio_info audio_in;
    bool have_in = audio_source_read(&audio_in);
    dc_net_info net;
    dc_net_wifi(&net);
    dc_bluez_info bt;
    bool have_bt = dc_bluez_read(&bt);
    char sink_name[64], source_name[64];
    read_audio_device_names(sink_name, sizeof(sink_name), source_name, sizeof(source_name));
    float brightness = read_brightness();

    /* --- Sliders: volume + brightness, side by side --------------------
     * While a slider is being dragged (docs/13-POPOUTS-SPEC.md sec.1), show
     * the dragged fraction directly rather than re-reading system state —
     * wpctl/brightnessctl round-trip async, so reading back mid-drag would
     * either show a stale value or race the write. */
    float vol_frac = (cc->slider_dragging && cc->slider_drag_slot == 0)
                         ? cc->slider_drag_value
                         : (have_out ? audio_out.volume / 100.0f : 0.5f);
    float bright_frac = (cc->slider_dragging && cc->slider_drag_slot == 1)
                            ? cc->slider_drag_value
                            : (brightness >= 0.0f ? brightness : 0.7f);
    draw_slider(cc->render, l.slot_x[0], l.sliders_y + l.slider_h / 2.0f, l.slot_w,
               DC_ICON_VOLUME_UP, vol_frac);
    draw_slider(cc->render, l.slot_x[1], l.sliders_y + l.slider_h / 2.0f, l.slot_w,
               DC_ICON_BRIGHTNESS_MEDIUM, bright_frac);

    /* --- Media transport row (docs/13-POPOUTS-SPEC.md sec.1 item 4), only
     * when an MPRIS player is present -- cc_get_layout() already reserved
     * (or didn't reserve) the vertical space for this via st.media_active. */
    if (st.media_active)
        draw_media_row(cc->render, &l, &st.media);

    /* --- Tile grid: wifi/bluetooth, audioOutput/audioInput, nightMode/
     * darkMode (order per the user's controlCenterWidgets config) -------- */
    char wifi_title[64], wifi_sub[32];
    if (net.connected) {
        snprintf(wifi_title, sizeof(wifi_title), "%s", net.ssid[0] ? net.ssid : "Connected");
        if (net.signal_percent >= 0)
            snprintf(wifi_sub, sizeof(wifi_sub), "%d%%", net.signal_percent);
        else
            snprintf(wifi_sub, sizeof(wifi_sub), "Connected");
    } else {
        snprintf(wifi_title, sizeof(wifi_title), "Wi-Fi");
        snprintf(wifi_sub, sizeof(wifi_sub), "%s", net.has_wifi ? "Disconnected" : "Unavailable");
    }
    draw_pill_tile(cc->render, cc_tile_x(&l, 0), cc_tile_y(&l, 0), l.tile_w, l.tile_h, DC_ICON_WIFI,
                  wifi_title, wifi_sub, net.connected);
    /* Expand chevron (docs/13-POPOUTS-SPEC.md sec.1 item 1): points right
     * when collapsed, down while the network list is open, painted on top
     * of the pill tile it shares. */
    dc_render_icon(cc->render, cc->net_expanded ? DC_ICON_EXPAND_MORE : DC_ICON_CHEVRON_RIGHT,
                  l.wifi_chevron_cx, l.wifi_chevron_cy, 16.0f, t->surface_variant_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    /* "active" (icon chip fill) tracks adapter *powered* state, not
     * device-connected -- matches the reference screenshot (chip is solid
     * green with 0 connected devices). Subtitle now uses the real device
     * count (bluez.c was extended with a device[] list for the expand
     * panel below) rather than the previous flat "Connected"/"No devices". */
    const char *bt_title = (have_bt && bt.powered) ? "Enabled" : "Disabled";
    char bt_sub[24];
    if (!have_bt || !bt.powered)
        snprintf(bt_sub, sizeof(bt_sub), "No devices");
    else if (bt.connected) {
        int n_conn = 0;
        for (int i = 0; i < bt.device_count; i++)
            n_conn += bt.devices[i].connected ? 1 : 0;
        snprintf(bt_sub, sizeof(bt_sub), "%d connected", n_conn);
    } else {
        snprintf(bt_sub, sizeof(bt_sub), "No devices");
    }
    draw_pill_tile(cc->render, cc_tile_x(&l, 1), cc_tile_y(&l, 0), l.tile_w, l.tile_h,
                  DC_ICON_BLUETOOTH, bt_title, bt_sub, have_bt && bt.powered);
    dc_render_icon(cc->render, cc->bt_expanded ? DC_ICON_EXPAND_MORE : DC_ICON_CHEVRON_RIGHT,
                  l.bt_chevron_cx, l.bt_chevron_cy, 16.0f, t->surface_variant_text,
                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);

    char out_title[64], out_sub[16];
    snprintf(out_title, sizeof(out_title), "%s", sink_name[0] ? sink_name : "Speakers");
    bool out_muted = have_out && audio_out.muted;
    snprintf(out_sub, sizeof(out_sub), "%s", out_muted ? "Muted" : "");
    if (!out_muted)
        snprintf(out_sub, sizeof(out_sub), "%d%%", have_out ? audio_out.volume : 0);
    draw_pill_tile(cc->render, cc_tile_x(&l, 0), cc_tile_y(&l, 1), l.tile_w, l.tile_h,
                  DC_ICON_VOLUME_UP, out_title, out_sub, !out_muted);

    char in_title[64], in_sub[16];
    snprintf(in_title, sizeof(in_title), "%s", source_name[0] ? source_name : "Microphone");
    bool in_muted = have_in && audio_in.muted;
    if (in_muted)
        snprintf(in_sub, sizeof(in_sub), "Muted");
    else
        snprintf(in_sub, sizeof(in_sub), "%d%%", have_in ? audio_in.volume : 0);
    draw_pill_tile(cc->render, cc_tile_x(&l, 1), cc_tile_y(&l, 1), l.tile_w, l.tile_h,
                  in_muted ? DC_ICON_MIC_OFF : DC_ICON_MIC, in_title, in_sub, !in_muted);

    /* nightMode: dankc has no real night-mode (gammastep) service hook wired
     * to a live toggle state yet (out of this task's scope, see docs/POLISH
     * P4 item order), so it stays the same hardcoded placeholder -- restyled
     * only. darkMode: dankc's stock themes (theme/stock_themes.inc) are all
     * DARK-only, there's no light variant to flip to (docs/13-POPOUTS-
     * SPEC.md sec.1 asked to wire this to "something real" if no light
     * variant exists) -- repurposed to toggle dynamic color (theme derived
     * from the wallpaper via theme/dynamic.cpp) instead, since that's the
     * other config-driven, already-wired palette switch (settings.c's
     * "Dynamic color" toggle does the exact same apply+save+notify). */
    draw_toggle_tile(cc->render, cc_tile_x(&l, 0), cc_tile_y(&l, 2), l.tile_w, l.tile_h,
                     DC_ICON_NIGHTLIGHT, "Night Mode", false);
    draw_toggle_tile(cc->render, cc_tile_x(&l, 1), cc_tile_y(&l, 2), l.tile_w, l.tile_h,
                     DC_ICON_CONTRAST, "Dark Mode", dc_config_current->dynamic_color);

    /* --- Expand panel: network scan list or bluetooth device list
     * (docs/13-POPOUTS-SPEC.md sec.1 items 1/2), mutually exclusive with
     * each other -- cc_get_layout() already sized the card to fit whichever
     * is open. */
    if (l.expand_kind != 0) {
        nvgFontFaceId(vg, cc->render->font_ui);
        nvgFontSize(vg, 12.0f);
        nvgFillColor(vg, tc(t->surface_variant_text));
        nvgTextAlign(vg, NVG_ALIGN_LEFT | NVG_ALIGN_MIDDLE);
        nvgText(vg, l.ix, l.expand_header_y, l.expand_kind == 1 ? "NETWORKS" : "BLUETOOTH DEVICES",
               NULL);

        /* "Discover" toggle (W3.1): starts/stops Adapter1 discovery so
         * unpaired nearby devices show up in the list below. "Discoverable"
         * (W1 polish): makes *this machine* visible to other devices'
         * scans -- independent of whether dankc itself is scanning. */
        if (l.expand_kind == 2) {
            bool discovering = st.bt_discovering;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, l.bt_scan_btn_x0, l.bt_scan_btn_y0,
                          l.bt_scan_btn_x1 - l.bt_scan_btn_x0, l.bt_scan_btn_h,
                          l.bt_scan_btn_h / 2.0f);
            nvgFillColor(vg, discovering ? tc(t->primary) : tc(t->surface_container_high));
            nvgFill(vg);
            nvgFontSize(vg, 10.0f);
            nvgFillColor(vg, tc(discovering ? t->primary_text : t->surface_text));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, (l.bt_scan_btn_x0 + l.bt_scan_btn_x1) / 2.0f,
                   l.bt_scan_btn_y0 + l.bt_scan_btn_h / 2.0f, discovering ? "Stop" : "Discover",
                   NULL);

            bool discoverable = st.bt_discoverable;
            nvgBeginPath(vg);
            nvgRoundedRect(vg, l.bt_disc_btn_x0, l.bt_disc_btn_y0,
                          l.bt_disc_btn_x1 - l.bt_disc_btn_x0, l.bt_disc_btn_h,
                          l.bt_disc_btn_h / 2.0f);
            nvgFillColor(vg, discoverable ? tc(t->primary) : tc(t->surface_container_high));
            nvgFill(vg);
            nvgFontSize(vg, 10.0f);
            nvgFillColor(vg, tc(discoverable ? t->primary_text : t->surface_text));
            nvgTextAlign(vg, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
            nvgText(vg, (l.bt_disc_btn_x0 + l.bt_disc_btn_x1) / 2.0f,
                   l.bt_disc_btn_y0 + l.bt_disc_btn_h / 2.0f, discoverable ? "Visible" : "Hidden",
                   NULL);
        }

        if (st.expand_rows == 0) {
            nvgFontSize(vg, 13.0f);
            nvgFillColor(vg, tc(t->surface_variant_text));
            nvgText(vg, l.ix, cc_expand_row_y(&l, 0) + l.expand_row_h / 2.0f,
                   l.expand_kind == 1 ? "Scanning\xe2\x80\xa6"
                   : st.bt_discovering  ? "Searching\xe2\x80\xa6"
                                        : "No paired devices",
                   NULL);
        }

        for (int i = 0; i < st.expand_rows && i < CC_MAX_EXPAND_ROWS; i++) {
            float ry = cc_expand_row_y(&l, i);
            float cy = ry + l.expand_row_h / 2.0f;
            if (l.expand_kind == 1) {
                const dc_net_wifi_ap *ap = &st.net_aps[i];
                bool pw_open = i == l.pw_after_row;
                bool saved = st.net_ap_saved[i];
                char status[24];
                status[0] = '\0';
                if (pw_open) {
                    /* The panel drawn right below already shows
                     * blank/"Connecting..."/error -- no need to repeat it. */
                } else if (ap->in_use)
                    snprintf(status, sizeof(status), "Connected");
                else if (ap->secured)
                    snprintf(status, sizeof(status), "%d%%", ap->signal_percent);
                else
                    snprintf(status, sizeof(status), "Open \xc2\xb7 %d%%", ap->signal_percent);
                /* Saved-network badge (W2): a small pin icon between the
                 * title and status text. Forget action icon (only when not
                 * mid password-entry -- cc_close_net_pw() already tears down
                 * the panel before a forget would make sense anyway). */
                bool show_forget = saved && !pw_open;
                draw_expand_row(cc->render, l.ix, ry, l.iw, l.expand_row_h,
                                ap->secured ? DC_ICON_LOCK : DC_ICON_WIFI, ap->ssid, status,
                                ap->in_use, false, NULL, saved ? DC_ICON_PUSH_PIN : 0,
                                show_forget ? CC_ROW_FORGET_RESERVE_W : 0.0f);
                if (show_forget)
                    dc_render_icon(cc->render, DC_ICON_CLOSE, l.net_forget_cx, cy, 13.0f,
                                  t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                if (pw_open)
                    draw_net_pw_panel(cc->render, &l, cc->net_pw_buf, cc->net_pw_connecting,
                                      cc->net_pw_err);
            } else {
                const dc_bluez_device *d = &st.bt_devs[i];
                bool this_job = st.bt_pair_state != DC_BLUEZ_PAIR_IDLE &&
                                strcmp(st.bt_pair_mac, d->mac) == 0;
                int icon = cc_bt_device_icon(d);
                char status[24];
                bool status_primary = false, status_warn = false;
                if (this_job && st.bt_pair_state == DC_BLUEZ_PAIR_IN_PROGRESS)
                    snprintf(status, sizeof(status), "Pairing\xe2\x80\xa6");
                else if (this_job && st.bt_pair_state == DC_BLUEZ_PAIR_FAILED) {
                    snprintf(status, sizeof(status), "Failed");
                    status_warn = true;
                } else if (this_job && st.bt_pair_state == DC_BLUEZ_PAIR_SUCCESS) {
                    snprintf(status, sizeof(status), "Paired");
                    status_primary = true;
                } else if (d->connected) {
                    snprintf(status, sizeof(status), "Connected");
                    status_primary = true;
                } else if (d->paired) {
                    snprintf(status, sizeof(status), "Tap to connect");
                } else {
                    snprintf(status, sizeof(status), "Tap to pair");
                }
                char batt[8] = "";
                if (d->battery_percent >= 0)
                    snprintf(batt, sizeof(batt), "%d%%", d->battery_percent);
                draw_expand_row(cc->render, l.ix, ry, l.iw, l.expand_row_h, icon, d->name, status,
                                status_primary, status_warn, batt[0] ? batt : NULL, 0,
                                d->paired ? CC_ROW_BT_ACTIONS_RESERVE_W : 0.0f);
                if (d->paired) {
                    /* Trust toggle: same STAR glyph, primary-tinted when
                     * trusted (matches draw_toggle_tile()'s "one glyph, two
                     * colors" active/inactive convention). Remove/unpair. */
                    dc_render_icon(cc->render, DC_ICON_STAR, l.bt_trust_cx, cy, 14.0f,
                                  d->trusted ? t->primary : t->surface_variant_text,
                                  NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                    dc_render_icon(cc->render, DC_ICON_CLOSE, l.bt_remove_cx, cy, 13.0f,
                                  t->surface_variant_text, NVG_ALIGN_CENTER | NVG_ALIGN_MIDDLE);
                }
            }
        }

        if (l.bt_agent_active)
            draw_bt_agent_panel(cc->render, &l, st.bt_agent_device, st.bt_agent_passkey,
                                cc->bt_passkey_buf);
    }

    draw_cc_hover(cc, &l);

    nvgEndFrame(vg);

    /* Keep requesting frame callbacks while a password-entry connect job or a
     * bluetooth pairing job is in flight, purely to keep re-polling their
     * respective poll functions until they resolve -- otherwise the
     * "Connecting.../Pairing..." row would only ever update on the next
     * pointer click/motion (docs/13-POPOUTS-SPEC.md sec.1). */
    if ((dc_anim_active(&cc->anim) || cc->closing || cc->net_pw_connecting ||
        cc->bt_pairing_active) &&
       !cc->frame_cb) {
        cc->frame_cb = wl_surface_frame(cc->surface);
        wl_callback_add_listener(cc->frame_cb, &cc_frame_listener, cc);
    }
    dc_egl_swap(cc->egl, &cc->egl_window);
}

static void fractional_scale_handle_preferred(void *data, struct wp_fractional_scale_v1 *fs,
                                              uint32_t scale)
{
    dc_control_center *cc = data;
    DC_UNUSED(fs);
    cc->scale120 = (int)scale;
    recompute_physical(cc);
    cc_render(cc);
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

static void layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *surface,
                                            uint32_t serial, uint32_t width, uint32_t height)
{
    dc_control_center *cc = data;
    zwlr_layer_surface_v1_ack_configure(surface, serial);
    cc->logical_width = width > 0 ? (int)width : DC_CC_WIDTH;
    cc->logical_height = height > 0 ? (int)height : DC_CC_HEIGHT;
    cc->configured = true;
    recompute_physical(cc);
    cc_render(cc);
}

static void layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *surface)
{
    dc_control_center *cc = data;
    DC_UNUSED(surface);
    cc->configured = false;
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

dc_control_center *dc_control_center_create(dc_wayland *wl, dc_egl *egl, dc_render *render)
{
    dc_control_center *cc = calloc(1, sizeof(*cc));
    cc->wl = wl;
    cc->egl = egl;
    cc->render = render;
    cc->logical_width = DC_CC_WIDTH;
    cc->logical_height = DC_CC_HEIGHT;
    cc->scale120 = DC_SCALE_BASE;
    return cc;
}

static void cc_show(dc_control_center *cc, dc_output *output)
{
    cc->output = output;
    cc->configured = false;
    cc->egl_ready = false;
    cc->scale120 = (output && output->scale > 0 ? output->scale : 1) * DC_SCALE_BASE;
    dc_anim_start(&cc->anim, DC_DUR_MEDIUM, DC_EASE_EXPRESSIVE);

    cc->surface = wl_compositor_create_surface(cc->wl->compositor);
    if (cc->wl->fractional_scale_mgr) {
        cc->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            cc->wl->fractional_scale_mgr, cc->surface);
        wp_fractional_scale_v1_add_listener(cc->fractional_scale, &fractional_scale_listener, cc);
    }
    if (cc->wl->viewporter)
        cc->viewport = wp_viewporter_get_viewport(cc->wl->viewporter, cc->surface);

    cc->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        cc->wl->layer_shell, cc->surface, output ? output->wl_output : NULL,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "dankc:control-center");

    /* Bar-adjacent, right-aligned (docs/13-POPOUTS-SPEC.md sec.0/1: CC opens
     * near the bar's right cluster, on whichever screen edge the bar is on). */
    dc_popout_anchor pa =
        dc_popout_bar_adjacent(dc_config_current, DC_POPOUT_ALIGN_END, DC_CC_SIDE_MARGIN);
    cc->anim_ox = pa.origin_x;
    cc->anim_oy = pa.origin_y;
    zwlr_layer_surface_v1_set_anchor(cc->layer_surface, pa.anchor);
    zwlr_layer_surface_v1_set_size(cc->layer_surface, DC_CC_WIDTH, DC_CC_HEIGHT);
    zwlr_layer_surface_v1_set_margin(cc->layer_surface, pa.margin_top, pa.margin_right,
                                     pa.margin_bottom, pa.margin_left);
    zwlr_layer_surface_v1_set_exclusive_zone(cc->layer_surface, -1);
    /* On-demand keyboard (W1.1) so the inline Wi-Fi password field can be
     * typed into without permanently stealing focus from other apps while
     * the panel is just sitting open for mouse use -- same rationale as
     * settings.c's text fields. */
    zwlr_layer_surface_v1_set_keyboard_interactivity(
        cc->layer_surface, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_ON_DEMAND);
    zwlr_layer_surface_v1_add_listener(cc->layer_surface, &layer_surface_listener, cc);

    wl_surface_commit(cc->surface);
    cc->visible = true;
    cc->closing = false;
    dc_debug("control center shown");
}

static void cc_teardown(dc_control_center *cc)
{
    if (cc->frame_cb) {
        wl_callback_destroy(cc->frame_cb);
        cc->frame_cb = NULL;
    }
    if (cc->egl_ready)
        dc_egl_window_finish(&cc->egl_window, cc->egl);
    if (cc->viewport)
        wp_viewport_destroy(cc->viewport);
    if (cc->fractional_scale)
        wp_fractional_scale_v1_destroy(cc->fractional_scale);
    if (cc->layer_surface)
        zwlr_layer_surface_v1_destroy(cc->layer_surface);
    if (cc->surface)
        wl_surface_destroy(cc->surface);
    cc->egl_ready = false;
    cc->configured = false;
    cc->viewport = NULL;
    cc->fractional_scale = NULL;
    cc->layer_surface = NULL;
    cc->surface = NULL;
    cc->visible = false;
    cc->closing = false;
    cc->hover_id = CC_HOVER_NONE;
    cc->slider_dragging = false;
    /* Next open starts collapsed (docs/13-POPOUTS-SPEC.md sec.1 items 1/2) --
     * matches every other popout's "closed == reset" convention. */
    cc->net_expanded = false;
    cc_close_bt_section(cc);
    cc_close_net_pw(cc);
    dc_debug("control center hidden");
}

static void cc_begin_close(dc_control_center *cc)
{
    if (!cc->visible || cc->closing)
        return;
    dc_anim_start(&cc->anim, DC_DUR_SHORT, DC_EASE_EMPHASIZED_ACCEL);
    cc->closing = true;
    if (!dc_anim_active(&cc->anim)) {
        cc_teardown(cc);
        return;
    }
    cc_render(cc);
}

void dc_control_center_toggle(dc_control_center *cc, dc_output *output)
{
    if (cc->visible)
        cc_begin_close(cc);
    else
        cc_show(cc, output);
}

bool dc_control_center_visible(dc_control_center *cc)
{
    return cc->visible;
}

void dc_control_center_hide(dc_control_center *cc)
{
    cc_begin_close(cc);
}

struct wl_surface *dc_control_center_surface(dc_control_center *cc)
{
    return cc->surface;
}

void dc_control_center_handle_click(dc_control_center *cc, double x, double y)
{
    if (!cc->visible || cc->closing)
        return;

    cc_state st;
    cc_gather_state(cc, &st);
    bool net_row_saved[CC_MAX_EXPAND_ROWS], bt_row_paired[CC_MAX_EXPAND_ROWS];
    cc_row_action_flags(&st, net_row_saved, bt_row_paired);
    cc_layout l = cc_get_layout((float)cc->logical_width, st.media_active, st.expand_kind,
                                st.expand_rows, cc_find_pw_row(cc, &st), st.bt_agent_kind,
                                net_row_saved, bt_row_paired);

    /* Header action buttons: lock, power, settings, edit. */
    for (int i = 0; i < 4; i++) {
        double dx = x - (double)l.btn_cx[i];
        double dy = y - (double)l.btn_cy;
        if (dx * dx + dy * dy <= (double)(l.btn_r * l.btn_r)) {
            switch (i) {
            case 0: /* lock */
                run_self_ctl("lock");
                break;
            case 1: /* power */
                /* TODO(P4-power): power menu modal not yet implemented
                 * (QML: ControlCenterPopout.qml's powerMenuModalLoader /
                 * Components/PowerButton.qml). */
                dc_debug("control center: power button (power menu TODO)");
                break;
            case 2: /* settings */
                run_self_ctl("settings");
                break;
            case 3: /* edit */
                /* TODO: tile edit/reorder mode (EditControls.qml) not
                 * implemented -- dankc's tile grid/order is config-driven,
                 * not user-editable at runtime yet. */
                dc_debug("control center: edit button (edit mode TODO)");
                break;
            }
            return;
        }
    }

    /* Sliders: volume (slot 0) / brightness (slot 1). Click-to-set applies
     * immediately; the press also arms a drag (docs/13-POPOUTS-SPEC.md
     * sec.1) so motion keeps updating the value live until button release
     * (dc_control_center_handle_motion() / dc_control_center_handle_release()). */
    for (int i = 0; i < 2; i++) {
        if (x < (double)l.slot_x[i] || x > (double)(l.slot_x[i] + l.slot_w))
            continue;
        if (y < (double)l.sliders_y || y > (double)(l.sliders_y + l.slider_h))
            continue;

        float frac = cc_slider_frac_at(&l, i, x);
        if (i == 0)
            cc_apply_volume_frac(frac);
        else
            cc_apply_brightness_frac(frac);

        cc->slider_dragging = true;
        cc->slider_drag_slot = i;
        cc->slider_drag_value = frac;

        cc_render(cc);
        return;
    }

    /* Wifi/bluetooth expand chevrons (docs/13-POPOUTS-SPEC.md sec.1 items
     * 1/2) -- checked before the tile grid below since the chevron sits
     * inside the wifi/bluetooth tile's own bounds (top-right corner). Only
     * one panel is ever open: picking one closes the other and clears any
     * pending password hint. */
    dc_info("TEMPDBG click x=%f y=%f wifi_cx=%f wifi_cy=%f bt_cx=%f bt_cy=%f r=%f", x, y,
           (double)l.wifi_chevron_cx, (double)l.wifi_chevron_cy, (double)l.bt_chevron_cx,
           (double)l.bt_chevron_cy, (double)l.chevron_r);
    {
        double dx = x - (double)l.wifi_chevron_cx, dy = y - (double)l.wifi_chevron_cy;
        if (dx * dx + dy * dy <= (double)(l.chevron_r * l.chevron_r)) {
            cc->net_expanded = !cc->net_expanded;
            cc_close_bt_section(cc);
            cc_close_net_pw(cc);
            cc_render(cc);
            return;
        }
        dx = x - (double)l.bt_chevron_cx;
        dy = y - (double)l.bt_chevron_cy;
        if (dx * dx + dy * dy <= (double)(l.chevron_r * l.chevron_r)) {
            bool now_open = !cc->bt_expanded;
            cc->net_expanded = false;
            cc_close_net_pw(cc);
            if (now_open)
                cc->bt_expanded = true;
            else
                cc_close_bt_section(cc);
            cc_render(cc);
            return;
        }
    }

    /* Bluetooth "Discover" toggle (W3.1) -- starts/stops Adapter1 discovery
     * so unpaired nearby devices show up in the list below. */
    if (l.expand_kind == 2 && x >= (double)l.bt_scan_btn_x0 && x <= (double)l.bt_scan_btn_x1 &&
       y >= (double)l.bt_scan_btn_y0 && y <= (double)(l.bt_scan_btn_y0 + l.bt_scan_btn_h)) {
        if (st.bt_discovering)
            dc_bluez_stop_discovery();
        else
            dc_bluez_start_discovery();
        cc_render(cc);
        return;
    }

    /* Adapter "Discoverable" toggle (W1 polish) -- makes this machine visible
     * to other devices' scans, independent of dc_bluez_discovering() above. */
    if (l.expand_kind == 2 && x >= (double)l.bt_disc_btn_x0 && x <= (double)l.bt_disc_btn_x1 &&
       y >= (double)l.bt_disc_btn_y0 && y <= (double)(l.bt_disc_btn_y0 + l.bt_disc_btn_h)) {
        dc_bluez_set_discoverable(!st.bt_discoverable);
        cc_render(cc);
        return;
    }

    /* Media transport row (docs/13-POPOUTS-SPEC.md sec.1 item 4). */
    if (l.media_active) {
        for (int i = 0; i < 3; i++) {
            double dx = x - (double)l.media_btn_cx[i], dy = y - (double)l.media_btn_cy;
            if (dx * dx + dy * dy > (double)(l.media_btn_r * l.media_btn_r))
                continue;
            if (i == 0)
                dc_mpris_previous();
            else if (i == 1)
                dc_mpris_play_pause();
            else
                dc_mpris_next();
            cc_render(cc);
            return;
        }
    }

    /* Tile grid: wifi/bluetooth toggle rfkill (unchanged from before);
     * audioOutput/audioInput toggle mute via the same wpctl already used by
     * the sliders above (new, but reuses the exact tool/pattern -- these
     * tiles didn't exist as clickable elements before this task); darkMode
     * toggles dynamic color (see cc_render()'s comment); nightMode stays a
     * no-op (no backing service yet). */
    for (int row = 0; row < 3; row++) {
        float ry = cc_tile_y(&l, row);
        if (y < (double)ry || y > (double)(ry + l.tile_h))
            continue;
        for (int col = 0; col < 2; col++) {
            float rx = cc_tile_x(&l, col);
            if (x < (double)rx || x > (double)(rx + l.tile_w))
                continue;
            if (row == 0 && col == 0)
                run_detached("rfkill toggle wifi");
            else if (row == 0 && col == 1)
                run_detached("rfkill toggle bluetooth");
            else if (row == 1 && col == 0)
                run_detached("wpctl set-mute @DEFAULT_AUDIO_SINK@ toggle");
            else if (row == 1 && col == 1)
                run_detached("wpctl set-mute @DEFAULT_AUDIO_SOURCE@ toggle");
            else if (row == 2 && col == 1) {
                dc_config *cfg = dc_config_mut();
                cfg->dynamic_color = !cfg->dynamic_color;
                dc_config_reapply();
                dc_config_save();
                dc_config_notify_changed();
            } else
                dc_debug("control center: night toggle (no-op)");
            cc_render(cc);
            return;
        }
    }

    /* Expand panel rows: connect/disconnect a bluetooth device, or connect
     * to an open/known Wi-Fi network (unchanged, one-click). A secured,
     * not-yet-known SSID instead opens the inline password panel (W1.1)
     * right below its row -- see draw_net_pw_panel()/cc_get_layout(). */
    if (l.expand_kind != 0 && st.expand_rows > 0) {
        int rows = st.expand_rows > CC_MAX_EXPAND_ROWS ? CC_MAX_EXPAND_ROWS : st.expand_rows;
        for (int i = 0; i < rows; i++) {
            float ry = cc_expand_row_y(&l, i);
            if (y < (double)ry || y > (double)(ry + l.expand_row_h))
                continue;

            /* Per-row action icons (W1/W2 polish) -- checked (by circular hit
             * test, independent of the row's x-bound check below) before the
             * connect/pair/forget-target dispatch further down, same priority
             * order as cc_hittest(). */
            float cy = ry + l.expand_row_h / 2.0f;
            if (l.expand_kind == 1 && st.net_ap_saved[i]) {
                double dx = x - (double)l.net_forget_cx, dy = y - (double)cy;
                if (dx * dx + dy * dy <= (double)(l.net_forget_r * l.net_forget_r)) {
                    dc_net_saved_forget(st.net_ap_saved_path[i]);
                    cc_render(cc);
                    return;
                }
            } else if (l.expand_kind == 2 && st.bt_devs[i].paired) {
                double dx = x - (double)l.bt_trust_cx, dy = y - (double)cy;
                if (dx * dx + dy * dy <= (double)(l.bt_action_r * l.bt_action_r)) {
                    dc_bluez_set_trusted(st.bt_devs[i].mac, !st.bt_devs[i].trusted);
                    cc_render(cc);
                    return;
                }
                dx = x - (double)l.bt_remove_cx;
                if (dx * dx + dy * dy <= (double)(l.bt_action_r * l.bt_action_r)) {
                    dc_bluez_remove(st.bt_devs[i].mac);
                    cc_render(cc);
                    return;
                }
            }

            if (x < (double)l.ix || x > (double)(l.ix + l.iw))
                continue;

            if (l.expand_kind == 1) {
                const dc_net_wifi_ap *ap = &st.net_aps[i];
                if (ap->in_use) {
                    /* already connected -- nothing to do */
                } else if (!ap->secured || ap->known) {
                    cc_close_net_pw(cc);
                    dc_net_wifi_connect(ap->ssid);
                } else if (!cc->net_pw_active || strcmp(cc->net_pw_ssid, ap->ssid) != 0) {
                    /* A different SSID's panel (or none) was open -- (re)open
                     * it fresh for this one. Re-clicking the *same* SSID's
                     * already-open row is a no-op (keeps whatever's typed). */
                    cc_close_net_pw(cc);
                    cc->net_pw_active = true;
                    snprintf(cc->net_pw_ssid, sizeof(cc->net_pw_ssid), "%s", ap->ssid);
                }
            } else {
                const dc_bluez_device *d = &st.bt_devs[i];
                if (d->paired) {
                    if (d->connected)
                        dc_bluez_disconnect(d->mac);
                    else
                        dc_bluez_connect(d->mac);
                } else {
                    /* Nearby, unpaired (W3.1) -- kick off Pair+Trust+Connect;
                     * the row flips to "Pairing.../Paired/Failed" via
                     * st.bt_pair_state on the next render (dc_bluez_pair()
                     * replaces any previous job, same "one at a time"
                     * convention as services/net.c's connect_psk job). */
                    dc_bluez_pair(d->mac);
                    cc->bt_pairing_active = true;
                }
            }
            cc_render(cc);
            return;
        }
    }

    /* Inline Wi-Fi password entry (W1.1): field (no-op on click -- keyboard
     * focus is implicit whenever net_pw_active is true) + Cancel/Connect. */
    if (l.pw_after_row >= 0) {
        bool in_field = x >= (double)l.ix && x <= (double)(l.ix + l.iw) &&
                        y >= (double)l.pw_field_y && y <= (double)(l.pw_field_y + l.pw_field_h);
        bool in_btn_row = y >= (double)l.pw_btn_y0 && y <= (double)(l.pw_btn_y0 + l.pw_btn_h);
        if (in_field)
            return;
        if (in_btn_row && x >= (double)l.pw_cancel_x0 && x <= (double)l.pw_cancel_x1) {
            cc_close_net_pw(cc);
            cc_render(cc);
            return;
        }
        if (in_btn_row && x >= (double)l.pw_connect_x0 && x <= (double)l.pw_connect_x1) {
            if (!cc->net_pw_connecting && cc->net_pw_buf[0]) {
                dc_net_wifi_connect_psk(cc->net_pw_ssid, cc->net_pw_buf);
                cc->net_pw_connecting = true;
                cc->net_pw_err[0] = '\0';
            }
            cc_render(cc);
            return;
        }
    }

    /* Bluetooth pairing-agent panel (W3.1): field (no-op on click, same
     * "focus is implicit" contract as the Wi-Fi password field above) +
     * Reject/Confirm (or Cancel/Submit for a passkey/PIN-entry request). */
    if (l.bt_agent_active) {
        bool has_field = cc_bt_agent_has_field(l.bt_agent_kind);
        bool in_field = has_field && x >= (double)l.ix && x <= (double)(l.ix + l.iw) &&
                        y >= (double)l.bt_agent_field_y &&
                        y <= (double)(l.bt_agent_field_y + l.bt_agent_field_h);
        bool in_btn_row =
            y >= (double)l.bt_agent_btn_y0 && y <= (double)(l.bt_agent_btn_y0 + l.bt_agent_btn_h);
        if (in_field)
            return;
        if (in_btn_row && x >= (double)l.bt_agent_no_x0 && x <= (double)l.bt_agent_no_x1) {
            if (l.bt_agent_kind == DC_BLUEZ_AGENT_PASSKEY)
                dc_bluez_agent_respond_passkey(NULL);
            else if (l.bt_agent_kind == DC_BLUEZ_AGENT_PIN_CODE)
                dc_bluez_agent_respond_pin_code(NULL);
            else
                dc_bluez_agent_respond_yesno(false);
            cc->bt_passkey_buf[0] = '\0';
            cc_render(cc);
            return;
        }
        if (in_btn_row && x >= (double)l.bt_agent_yes_x0 && x <= (double)l.bt_agent_yes_x1) {
            if (l.bt_agent_kind == DC_BLUEZ_AGENT_PASSKEY) {
                if (cc->bt_passkey_buf[0])
                    dc_bluez_agent_respond_passkey(cc->bt_passkey_buf);
            } else if (l.bt_agent_kind == DC_BLUEZ_AGENT_PIN_CODE) {
                if (cc->bt_passkey_buf[0])
                    dc_bluez_agent_respond_pin_code(cc->bt_passkey_buf);
            } else {
                dc_bluez_agent_respond_yesno(true);
            }
            cc->bt_passkey_buf[0] = '\0';
            cc_render(cc);
            return;
        }
    }
}

/* Pointer motion over the panel (docs/13-POPOUTS-SPEC.md sec.1): while a
 * slider drag is armed (see dc_control_center_handle_click()), every motion
 * updates the value live; otherwise this is plain hover tracking, re-
 * rendering only when the hovered id changes — same guard pattern as
 * bar.c's dc_bar_pointer_motion(). */
void dc_control_center_handle_motion(dc_control_center *cc, double x, double y)
{
    if (!cc->visible || cc->closing)
        return;

    cc_state st;
    cc_gather_state(cc, &st);
    bool net_row_saved[CC_MAX_EXPAND_ROWS], bt_row_paired[CC_MAX_EXPAND_ROWS];
    cc_row_action_flags(&st, net_row_saved, bt_row_paired);
    cc_layout l = cc_get_layout((float)cc->logical_width, st.media_active, st.expand_kind,
                                st.expand_rows, cc_find_pw_row(cc, &st), st.bt_agent_kind,
                                net_row_saved, bt_row_paired);

    if (cc->slider_dragging) {
        float frac = cc_slider_frac_at(&l, cc->slider_drag_slot, x);
        cc->slider_drag_value = frac;
        if (cc->slider_drag_slot == 0)
            cc_apply_volume_frac(frac);
        else
            cc_apply_brightness_frac(frac);
        cc_render(cc);
        return;
    }

    cc_hover_id id = cc_hittest(&l, x, y);
    if (id == cc->hover_id)
        return; /* still the same element — nothing to repaint */

    cc->hover_id = id;
    dc_wayland_set_cursor(cc->wl, id != CC_HOVER_NONE ? DC_CURSOR_POINTER : DC_CURSOR_DEFAULT);
    cc_render(cc);
}

/* Left button released anywhere: ends a slider drag, if one was in progress
 * (docs/13-POPOUTS-SPEC.md sec.1). No-op otherwise — plain hover has nothing
 * to finalize. */
void dc_control_center_handle_release(dc_control_center *cc)
{
    if (!cc->slider_dragging)
        return;
    cc->slider_dragging = false;
    cc_render(cc);
}

/* Pointer left the panel entirely: clear hover (and, defensively, any
 * in-progress drag — the compositor delivers leave before a surface can be
 * destroyed out from under an active grab, but there's no harm being safe). */
void dc_control_center_handle_leave(dc_control_center *cc)
{
    cc->slider_dragging = false;
    if (cc->hover_id == CC_HOVER_NONE)
        return;
    cc->hover_id = CC_HOVER_NONE;
    dc_wayland_set_cursor(cc->wl, DC_CURSOR_DEFAULT);
    cc_render(cc);
}

/* True while dankc's bluetooth pairing agent (W3.1) has a pending
 * RequestPasskey/RequestPinCode request and the bluetooth section is open --
 * the only agent-panel kinds that need typed input (RequestConfirmation/
 * RequestAuthorization only need the Yes/No buttons, handled entirely by
 * dc_control_center_handle_click()). Cheap to poll (bluez.c just returns a
 * static flag), safe to call from both wants_keyboard() and handle_key(). */
static bool bt_wants_passkey_keyboard(dc_control_center *cc, dc_bluez_agent_request *out)
{
    if (!cc->bt_expanded)
        return false;
    if (!dc_bluez_agent_poll(out))
        return false;
    return cc_bt_agent_has_field(out->kind);
}

/* W1.1/W3.1: the inline Wi-Fi password field or a bluetooth RequestPasskey/
 * RequestPinCode prompt are the only things that ever want keyboard input --
 * same "keyboard on demand" contract as dc_settings_wants_keyboard(). */
bool dc_control_center_wants_keyboard(dc_control_center *cc)
{
    if (!cc->visible || cc->closing)
        return false;
    if (cc->net_pw_active)
        return true;
    dc_bluez_agent_request req;
    return bt_wants_passkey_keyboard(cc, &req);
}

void dc_control_center_handle_key(dc_control_center *cc, uint32_t keysym, const char *utf8)
{
    if (cc->net_pw_active) {
        switch (keysym) {
        case XKB_KEY_Escape:
            cc_close_net_pw(cc);
            cc_render(cc);
            return;
        case XKB_KEY_Return:
        case XKB_KEY_KP_Enter:
            if (!cc->net_pw_connecting && cc->net_pw_buf[0]) {
                dc_net_wifi_connect_psk(cc->net_pw_ssid, cc->net_pw_buf);
                cc->net_pw_connecting = true;
                cc->net_pw_err[0] = '\0';
            }
            cc_render(cc);
            return;
        case XKB_KEY_BackSpace: {
            size_t n = strlen(cc->net_pw_buf);
            if (n > 0)
                cc->net_pw_buf[n - 1] = '\0';
            cc_render(cc);
            return;
        }
        default:
            /* Append the whole UTF-8 sequence for the key, same control-char
             * filter as launcher.c's query editing -- passwords may contain
             * any printable character, not just ASCII. */
            if (utf8 && utf8[0] && !((unsigned char)utf8[0] < 0x20) &&
                (unsigned char)utf8[0] != 0x7f) {
                size_t n = strlen(cc->net_pw_buf);
                size_t add = strlen(utf8);
                if (n + add < sizeof(cc->net_pw_buf)) {
                    memcpy(cc->net_pw_buf + n, utf8, add + 1);
                    cc_render(cc);
                }
            }
            return;
        }
    }

    dc_bluez_agent_request req;
    if (!bt_wants_passkey_keyboard(cc, &req))
        return;
    bool is_pin = req.kind == DC_BLUEZ_AGENT_PIN_CODE;
    /* Legacy PIN codes are capped at 16 chars per the Bluetooth spec; a
     * passkey is always exactly 6 digits -- either way, comfortably inside
     * bt_passkey_buf's 24-byte capacity. */
    size_t max_len = is_pin ? 16 : 6;

    switch (keysym) {
    case XKB_KEY_Escape:
        if (is_pin)
            dc_bluez_agent_respond_pin_code(NULL);
        else
            dc_bluez_agent_respond_passkey(NULL);
        cc->bt_passkey_buf[0] = '\0';
        cc_render(cc);
        return;
    case XKB_KEY_Return:
    case XKB_KEY_KP_Enter:
        if (cc->bt_passkey_buf[0]) {
            if (is_pin)
                dc_bluez_agent_respond_pin_code(cc->bt_passkey_buf);
            else
                dc_bluez_agent_respond_passkey(cc->bt_passkey_buf);
            cc->bt_passkey_buf[0] = '\0';
        }
        cc_render(cc);
        return;
    case XKB_KEY_BackSpace: {
        size_t n = strlen(cc->bt_passkey_buf);
        if (n > 0)
            cc->bt_passkey_buf[n - 1] = '\0';
        cc_render(cc);
        return;
    }
    default:
        /* RequestPasskey: digits only (a BlueZ passkey is always a 6-digit
         * decimal code). RequestPinCode (legacy pre-SSP pairing): any single
         * printable, non-control ASCII char -- the Bluetooth spec allows an
         * arbitrary PIN, not just digits. */
        if (utf8 && utf8[0] && utf8[1] == '\0' &&
            (is_pin ? (isprint((unsigned char)utf8[0]) != 0) : isdigit((unsigned char)utf8[0]))) {
            size_t n = strlen(cc->bt_passkey_buf);
            if (n < max_len && n + 1 < sizeof(cc->bt_passkey_buf)) {
                cc->bt_passkey_buf[n] = utf8[0];
                cc->bt_passkey_buf[n + 1] = '\0';
                cc_render(cc);
            }
        }
        return;
    }
}

void dc_control_center_destroy(dc_control_center *cc)
{
    if (!cc)
        return;
    if (cc->visible)
        cc_teardown(cc);
    free(cc);
}
