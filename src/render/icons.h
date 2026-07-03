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
/* Full per-~15% tier set, matching DMS's Theme.getBatteryIcon() exactly
 * (quickshell/Common/Theme.qml). These codepoints are all present in the
 * full vendored font (assets/fonts/MaterialSymbolsRounded.ttf) — verified
 * directly via its cmap — they just weren't in icons.h yet, so the build-
 * time subset (scripts/subset-fonts.sh, which derives its codepoint list
 * from this file) never carried them. Re-run that script after editing
 * this list. */
#define DC_ICON_BATTERY_0_BAR 0xebdc
#define DC_ICON_BATTERY_1_BAR 0xebd9
#define DC_ICON_BATTERY_2_BAR 0xebe0
#define DC_ICON_BATTERY_3_BAR 0xebdd
#define DC_ICON_BATTERY_4_BAR 0xebe2
#define DC_ICON_BATTERY_5_BAR 0xebd4
#define DC_ICON_BATTERY_6_BAR 0xebd2
#define DC_ICON_BATTERY_ALERT 0xe19c
#define DC_ICON_BATTERY_CHARGING_FULL 0xe1a3
#define DC_ICON_BATTERY_CHARGING_90 0xf0a7
#define DC_ICON_BATTERY_CHARGING_80 0xf0a6
#define DC_ICON_BATTERY_CHARGING_60 0xf0a5
#define DC_ICON_BATTERY_CHARGING_50 0xf0a4
#define DC_ICON_BATTERY_CHARGING_30 0xf0a3
#define DC_ICON_BATTERY_CHARGING_20 0xf0a2
#define DC_ICON_VOLUME_UP 0xe050
#define DC_ICON_VOLUME_DOWN 0xe04d
#define DC_ICON_VOLUME_OFF 0xe04f
#define DC_ICON_MIC 0xe31d
#define DC_ICON_MIC_OFF 0xe02b
#define DC_ICON_NOTIFICATIONS 0xe7f5
#define DC_ICON_CONTENT_PASTE 0xe14f
#define DC_ICON_SETTINGS 0xe8b8
#define DC_ICON_SEARCH 0xe8b6
#define DC_ICON_APPS 0xe5c3
#define DC_ICON_MUSIC_NOTE 0xe405
#define DC_ICON_WIDGETS 0xe1bd
#define DC_ICON_POWER 0xf8c7
/* NOTE: was 0xe897, which parsing the bundled font's cmap directly shows is
 * unassigned (renders as blank space) -- not a valid Material Symbols
 * codepoint at all (0xe899 is "lock"; 0xe898 is "lock_open"). Fixed while
 * adding the Control Center header's lock button (docs/13-POPOUTS-SPEC.md
 * sec.1), which also fixes the pre-existing lock-screen icon (ui/lock.c). */
#define DC_ICON_LOCK 0xe899
#define DC_ICON_DARK_MODE 0xe51c
#define DC_ICON_LIGHT_MODE 0xe518
#define DC_ICON_CLOSE 0xe5cd
#define DC_ICON_DONE 0xe876
#define DC_ICON_CHEVRON_RIGHT 0xe5cc
#define DC_ICON_EXPAND_MORE 0xe5cf
#define DC_ICON_EDIT 0xf097
#define DC_ICON_PERSON 0xf0d3
/* Control Center's nightMode/darkMode toggle tiles use these two, not
 * DARK_MODE/LIGHT_MODE above (docs/13-POPOUTS-SPEC.md sec.1; matches
 * Modules/ControlCenter/Models/WidgetModel.qml's coreWidgetDefinitions). */
#define DC_ICON_NIGHTLIGHT 0xf03d
#define DC_ICON_CONTRAST 0xeb37
/* Control Center brightness slider icon (BrightnessSliderRow.qml). */
#define DC_ICON_BRIGHTNESS_MEDIUM 0xe1ae

/* music (media) transport (docs/12-BAR-SPEC.md sec.4/6). */
#define DC_ICON_SKIP_PREVIOUS 0xe045
#define DC_ICON_SKIP_NEXT 0xe044
#define DC_ICON_PLAY_ARROW 0xe037
#define DC_ICON_PAUSE 0xe034

/* weather set (docs/12-BAR-SPEC.md sec.4/6), matching
 * dc_weather_icon_name()'s returned names 1:1. NOTE: these codepoints are
 * absent from the build-time font subset (assets/fonts/MaterialSymbolsRounded.ttf
 * was previously a ~2200-glyph subset that happened to skip the whole weather
 * block) — the font was swapped for DMS's full variable font (still loaded
 * from the same path/filename) so these render instead of tofu. */
#define DC_ICON_CLEAR_DAY 0xf157
#define DC_ICON_CLEAR_NIGHT 0xf159
#define DC_ICON_PARTLY_CLOUDY_DAY 0xf172
#define DC_ICON_PARTLY_CLOUDY_NIGHT 0xf174
#define DC_ICON_CLOUD 0xf15c
#define DC_ICON_RAINY 0xf176
#define DC_ICON_WEATHER_SNOWY 0xe2cd
#define DC_ICON_THUNDERSTORM 0xebdb
#define DC_ICON_FOGGY 0xe818

/* cpuUsage / memUsage (docs/12-BAR-SPEC.md sec.4/6), matching DMS's
 * CpuMonitor.qml / RamMonitor.qml icon names exactly. */
#define DC_ICON_MEMORY 0xe322        /* cpuUsage */
#define DC_ICON_DEVELOPER_BOARD 0xe30d /* memUsage */

/* controlCenterButton bluetooth sub-icon, connected state. */
#define DC_ICON_BLUETOOTH_CONNECTED 0xe1a8

/* Notification Center + Clipboard headers' clear button (docs/13-POPOUTS-
 * SPEC.md sec.3/4, NotificationHeader.qml/ClipboardHeader.qml's
 * "delete_sweep"). NOT 0xea79 -- that codepoint exists in the font but is a
 * *tractor* glyph (verified on-screen); e16c is what the font's own
 * .codepoints file maps "delete_sweep" to. */
#define DC_ICON_DELETE_SWEEP 0xe16c

/* Dashboard popout ("DankDash", docs/13-POPOUTS-SPEC.md sec.5). Tab-bar icons
 * (dashboard/music_note/wallpaper/wb_sunny/settings), Overview cards
 * (schedule/device_thermostat), Weather stats grid (humidity_low/air/speed/
 * rainy/wb_twilight/bedtime) and calendar chevrons. All values from DMS's
 * MaterialSymbolsRounded[...].codepoints (the full variable font dankc now
 * bundles). */
#define DC_ICON_DASHBOARD 0xe871
#define DC_ICON_WALLPAPER 0xe1bc
#define DC_ICON_WB_SUNNY 0xe430
#define DC_ICON_SCHEDULE 0xefd6
#define DC_ICON_DEVICE_THERMOSTAT 0xe1ff
#define DC_ICON_HUMIDITY_LOW 0xf164
#define DC_ICON_AIR 0xefd8
#define DC_ICON_SPEED 0xe9e4
#define DC_ICON_WB_TWILIGHT 0xe1c6
#define DC_ICON_BEDTIME 0xf159 /* moon glyph; doubles as the sunset icon */
#define DC_ICON_FOLDER 0xe2c7
#define DC_ICON_CHEVRON_LEFT 0xe5cb

/* Clipboard History popout (docs/13-POPOUTS-SPEC.md sec.4, ClipboardHeader.qml
 * / ClipboardEntry.qml / ClipboardThumbnail.qml). All values taken from DMS's
 * own MaterialSymbolsRounded .codepoints file (the same font dankc bundles)
 * and confirmed mapped in the TTF's cmap directly -- the classic Material
 * Icons table disagrees for some names (e.g. push_pin is f10d here, while
 * e840 exists but is a *different* glyph; verified visually on-screen). */
#define DC_ICON_PUSH_PIN 0xf10d     /* per-entry pin/unpin */
#define DC_ICON_CONTENT_COPY 0xe14d /* text-entry fallback glyph */
#define DC_ICON_SUBJECT 0xe8d2      /* long-text-entry fallback glyph */
#define DC_ICON_IMAGE 0xe3f4        /* image-entry fallback glyph (thumbnail decode failure) */

/* Processes popout (docs/13-POPOUTS-SPEC.md; ProcessListPopout.qml /
 * ProcessesView.qml). SCHEDULE (uptime glyph) is defined in the dashboard
 * block above. */
#define DC_ICON_ANALYTICS 0xef3e /* header "Processes" icon */

/* Launcher (docs/13-POPOUTS-SPEC.md sec.6): section-header view-mode toggles
 * and footer source-filter pills. Values taken from DMS's own MaterialSymbols
 * .codepoints file, same convention as the clipboard block above. */
#define DC_ICON_VIEW_LIST 0xe8ef   /* list view (default, fully implemented) */
#define DC_ICON_GRID_VIEW 0xe9b0   /* grid view */
#define DC_ICON_VIEW_MODULE 0xe8f0 /* compact view (TODO, dim/disabled) */
/* FOLDER (footer "Files" pill) is defined in the dashboard block above. */
#define DC_ICON_EXTENSION 0xe87b   /* footer "Plugins" pill (TODO, dim/disabled) */

/* Power menu (docs/13-POPOUTS-SPEC.md; Modals/PowerMenuModal.qml
 * getActionData()). LOCK/BEDTIME/POWER(=power_settings_new) are already
 * defined above; these two round it out. Values from DMS's own
 * MaterialSymbolsRounded .codepoints file. */
#define DC_ICON_LOGOUT 0xe9ba
#define DC_ICON_RESTART_ALT 0xf053

/* Launcher polish (docs/POLISH.md P4): the calculator row and the "this app
 * has extra actions" affordance. Values from DMS's own MaterialSymbolsRounded
 * .codepoints file, same convention as the rest of this header. */
#define DC_ICON_CALCULATE 0xea5f

/* Settings window (docs task "settings full coverage"): sidebar tab icons +
 * a few control glyphs. ui/settings.c previously defined its own local IC_*
 * macros that were never migrated into this header, so subset-fonts.sh (which
 * only scans DC_ICON_* here) never picked most of them up -- confirmed by
 * parsing assets/fonts/MaterialSymbolsRounded.subset.ttf's cmap directly:
 * PALETTE/TOOLBAR/INFO/MONITOR/ADD/REMOVE/LINK were all silently rendering as
 * tofu. Fixed by moving them here (so they're covered by the next subset
 * regen) and having settings.c include this header directly. */
#define DC_ICON_PALETTE 0xe40a
#define DC_ICON_TOOLBAR 0xe9f7
#define DC_ICON_INFO 0xe88e
#define DC_ICON_MONITOR 0xef5b
#define DC_ICON_ADD 0xe145
#define DC_ICON_REMOVE 0xe15b
#define DC_ICON_LINK 0xe250
/* New tabs (Dock & Launcher, Audio; Network/Bluetooth reuse the existing
 * WIFI/BLUETOOTH glyphs above, Power reuses the existing POWER glyph). */
#define DC_ICON_DOCK_TO_BOTTOM 0xf7e6

/* Settings tabs added by docs/14-COMPLETION-PLAN.md W2 (OSD, Typography &
 * Motion, System, Locale, Default Apps, Theme & Colors). Values verified
 * present (non-tofu) in assets/fonts/MaterialSymbolsRounded.ttf's cmap via
 * fontTools, same convention as the block above (Default Apps reuses the
 * existing APPS glyph). */
#define DC_ICON_TUNE 0xe429         /* OSD tab */
#define DC_ICON_TEXT_FORMAT 0xe165  /* Typography & Motion tab */
#define DC_ICON_COMPUTER 0xe30a     /* System tab */
#define DC_ICON_LANGUAGE 0xe894     /* Locale tab */
#define DC_ICON_COLOR_LENS 0xe3b0   /* Theme & Colors tab */

#endif /* DC_RENDER_ICONS_H */
