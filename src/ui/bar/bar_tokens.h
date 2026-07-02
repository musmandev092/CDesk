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

#endif /* DC_UI_BAR_TOKENS_H */
