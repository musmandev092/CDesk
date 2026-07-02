/* bar_tokens.h — shared size/padding formulas for every bar widget
 * (docs/12-BAR-SPEC.md sec.1-3). One place to compute pill height, radius,
 * padding, icon size, and text size so widget code never hardcodes them.
 */
#ifndef DC_UI_BAR_TOKENS_H
#define DC_UI_BAR_TOKENS_H

#include "core/config.h"

#include <math.h>

/* Pill/chip height ("widgetThickness"): max(20, 26 + innerPadding*0.6). At the
 * default innerPadding=4 this is 28px — every BasePill's height. */
static inline float dc_bar_widget_thickness(const dc_config *cfg)
{
    return fmaxf(20.0f, 26.0f + (float)cfg->bar_inner_padding * 0.6f);
}

/* BasePill corner radius: half the thickness, i.e. a full stadium. */
static inline float dc_bar_pill_radius(const dc_config *cfg)
{
    return dc_bar_widget_thickness(cfg) / 2.0f;
}

/* Horizontal padding inside a pill: widgetPadding * widgetThickness/30 — 7px
 * at the defaults (docs/12-BAR-SPEC.md sec.2). */
static inline float dc_bar_hpad(const dc_config *cfg)
{
    return roundf((float)cfg->bar_widget_padding * dc_bar_widget_thickness(cfg) / 30.0f);
}

/* Icon size at a given DMS "offset" from barIconSize(): round(widgetThickness *
 * (21+offset)/28). Common offsets used on the bar: -6 => 15px (tray, weather
 * glyphs), -4 => 17px (launcher/battery/bell/clipboard/CC icons). */
static inline float dc_bar_icon_size(const dc_config *cfg, int offset)
{
    return roundf(dc_bar_widget_thickness(cfg) * (float)(21 + offset) / 28.0f);
}

/* Widget label text size ("fontSizeSmall") — fixed, not thickness-derived. */
#define DC_BAR_TEXT_SIZE 12.0f

/* Spacing between widgets within a section (docs/12-BAR-SPEC.md sec.2). */
#define DC_BAR_WIDGET_SPACING 4.0f

/* Generic "row gap" between sub-segments *within* a single widget's content
 * (clock's time/dot/date, focusedWindow's app/dot/title) — DMS's
 * Theme.spacingS (docs/12-BAR-SPEC.md sec.4 clock/focusedWindow). */
#define DC_BAR_ROW_GAP 8.0f

/* Clamp a requested corner radius so it never exceeds half of either side —
 * i.e. "radius R, stadium-clamped" (docs/12-BAR-SPEC.md sec.4 workspaceSwitcher
 * capsules / systemTray chips): small shapes still render as a true stadium
 * or circle instead of nvgRoundedRect's arcs overlapping past R > min(w,h)/2. */
static inline float dc_bar_clamp_radius(float radius, float w, float h)
{
    return fminf(radius, fminf(w, h) / 2.0f);
}

/* Workspace capsules (docs/12-BAR-SPEC.md sec.4 workspaceSwitcher): chip
 * height is the full widgetThickness (no extra vertical padding); active
 * width `max(thickness*1.05, iconSize(-6)*1.6)`, inactive `max(thickness*0.7,
 * iconSize(-6)*1.2)`, ceiled to land on the spec's 30/20px at the defaults. */
static inline float dc_bar_ws_active_width(const dc_config *cfg)
{
    float t = dc_bar_widget_thickness(cfg);
    float icon = dc_bar_icon_size(cfg, -6);
    return ceilf(fmaxf(t * 1.05f, icon * 1.6f));
}

static inline float dc_bar_ws_inactive_width(const dc_config *cfg)
{
    float t = dc_bar_widget_thickness(cfg);
    float icon = dc_bar_icon_size(cfg, -6);
    return ceilf(fmaxf(t * 0.7f, icon * 1.2f));
}

/* Gap between adjacent workspace capsules (docs/12-BAR-SPEC.md sec.4: "~4-6px"). */
#define DC_BAR_WS_SPACING 5.0f

/* Clock digit cell: each HH/MM digit gets a fixed-width slot (fontSize*0.6)
 * so the clock doesn't jitter horizontally as digits change (docs/12-BAR-SPEC.md
 * sec.4 clock). */
#define DC_BAR_CLOCK_DIGIT_FACTOR 0.6f

/* systemTray per-item chip (docs/12-BAR-SPEC.md sec.4): 21x21, icon at
 * barIconSize(-6), gap between chips, and the letter-fallback text size. */
#define DC_BAR_TRAY_CHIP 21.0f
#define DC_BAR_TRAY_GAP 4.0f
#define DC_BAR_TRAY_LETTER_SIZE 10.0f

/* notificationButton unread dot (docs/12-BAR-SPEC.md sec.4). */
#define DC_BAR_UNREAD_DOT 6.0f
#define DC_BAR_UNREAD_DOT_RADIUS 3.0f

#endif /* DC_UI_BAR_TOKENS_H */
