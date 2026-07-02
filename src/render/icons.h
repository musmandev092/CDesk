/* icons.h — Material Symbols Rounded codepoints (PUA).
 *
 * DankC draws icons by codepoint (nanovg/fontstash has no OpenType shaping, so
 * ligature-by-name won't work). Values are from the bundled font's .codepoints
 * map (assets/fonts/MaterialSymbolsRounded.ttf). Add more as widgets need them.
 */
#ifndef DC_RENDER_ICONS_H
#define DC_RENDER_ICONS_H

#define DC_ICON_WIFI 0xe63e
#define DC_ICON_NETWORK_WIFI 0xe1ba
#define DC_ICON_SIGNAL_WIFI_4_BAR 0xf065
#define DC_ICON_BLUETOOTH 0xe1a7
#define DC_ICON_BATTERY_FULL 0xe1a5
/* NOTE: the bundled font is a build-time subset and does NOT include the
 * numbered battery_1_bar..battery_6_bar glyphs (0xf09c-0xf0a1) that DMS's
 * full variable font ships — confirmed by parsing the TTF's cmap directly
 * (they're simply absent, rendering as blank space, not tofu). battery_0_bar
 * is the only "low" tier actually present, so that's what low-but-not-
 * critical battery falls back to (docs/12-BAR-SPEC.md sec.4/6). */
#define DC_ICON_BATTERY_0_BAR 0xebdc
#define DC_ICON_BATTERY_ALERT 0xe19c
#define DC_ICON_BATTERY_CHARGING_FULL 0xe1a3
#define DC_ICON_VOLUME_UP 0xe050
#define DC_ICON_VOLUME_DOWN 0xe04d
#define DC_ICON_VOLUME_OFF 0xe04f
#define DC_ICON_MIC 0xe31d
#define DC_ICON_NOTIFICATIONS 0xe7f5
#define DC_ICON_CONTENT_PASTE 0xe14f
#define DC_ICON_SETTINGS 0xe8b8
#define DC_ICON_SEARCH 0xe8b6
#define DC_ICON_APPS 0xe5c3
#define DC_ICON_MUSIC_NOTE 0xe405
#define DC_ICON_WIDGETS 0xe1bd
#define DC_ICON_POWER 0xf8c7
#define DC_ICON_LOCK 0xe897
#define DC_ICON_DARK_MODE 0xe51c
#define DC_ICON_LIGHT_MODE 0xe518
#define DC_ICON_CLOSE 0xe5cd
#define DC_ICON_DONE 0xe876
#define DC_ICON_CHEVRON_RIGHT 0xe5cc
#define DC_ICON_EXPAND_MORE 0xe5cf

#endif /* DC_RENDER_ICONS_H */
