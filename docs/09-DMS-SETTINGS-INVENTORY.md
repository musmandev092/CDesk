# DankC — Complete DMS Settings Inventory (all 424 options)

Extracted verbatim from DankMaterialShell `translations/settings_search_index.json` (the authoritative
list of every user-facing setting). Format: **Label** — `configKey` — description [gate].
Use this as the checklist for DankC's settings tabs (`08-SETTINGS-UI.md`).

## Personalization  (36)

- **Automatic Cycling** — `wallpaperCyclingEnabled` — Automatically cycle through wallpapers in the same folder
- **Blur Wallpaper Layer** — `blurWallpaper` — Enable compositor-targetable blur layer (namespace: dms:blurwallpaper). Requires manual niri configuration.  _[isNiri]_
- **Blur on Overview** — `blurWallpaperOnOverview` — Blur wallpaper when niri overview is open  _[isNiri]_
- **Disable Built-in Wallpapers** — `disableWallpapers` — Use an external wallpaper manager like swww, hyprpaper, or swaybg.
- **Duplicate Wallpaper with Blur** — `blurredWallpaperLayer` — Enable compositor-targetable blur layer (namespace: dms:blurwallpaper). Requires manual niri configuration.
- **External Wallpaper Management** — `disableWallpaper` — Use an external wallpaper manager like swww, hyprpaper, or swaybg.
- **Interval** — `wallpaperCyclingInterval` — How often to change wallpaper
- **Matugen Target Monitor** — `matugenTargetMonitor` — Monitor whose wallpaper drives dynamic theming colors
- **Per-Mode Wallpapers** — `perModeWallpaper` — Set different wallpapers for light and dark mode
- **Per-Monitor Wallpapers** — `perMonitorWallpaper` — Set different wallpapers for each connected monitor
- **Personalization** — `_tab_0`
- **Transition Effect** — `wallpaperTransition` — Visual effect used when wallpaper changes
- **Wallpaper** — `wallpaper` — Color shown for areas not covered by wallpaper
- **Wallpaper Monitor** — `selectedMonitor` — Select monitor to configure wallpaper
- **Border Size** — `hyprlandLayoutBorderSize` — Width of window border
- **Border Size** — `mangoLayoutBorderSize` — Width of window border
- **Border Size** — `niriLayoutBorderSize` — Width of window border and focus ring
- **Hyprland Layout Overrides** — `hyprlandLayout` — Use custom gaps instead of bar spacing  _[isHyprland]_
- **MangoWC Layout Overrides** — `mangoLayout` — Use custom gaps instead of bar spacing  _[isMango]_
- **Niri Layout Overrides** — `niriLayout` — Use custom gaps instead of bar spacing  _[isNiri]_
- **Override Border Size** — `hyprlandLayoutBorderSizeEnabled` — Use custom border size
- **Override Border Size** — `mangoLayoutBorderSizeEnabled` — Use custom border size
- **Override Border Size** — `niriLayoutBorderSizeEnabled` — Use custom border/focus-ring width
- **Override Corner Radius** — `hyprlandLayoutRadiusOverrideEnabled` — Use custom window radius instead of theme radius
- **Override Corner Radius** — `mangoLayoutRadiusOverrideEnabled` — Use custom window radius instead of theme radius
- **Override Corner Radius** — `niriLayoutRadiusOverrideEnabled` — Use custom window radius instead of theme radius
- **Override Gaps** — `hyprlandLayoutGapsOverrideEnabled` — Use custom gaps instead of bar spacing
- **Override Gaps** — `mangoLayoutGapsOverrideEnabled` — Use custom gaps instead of bar spacing
- **Override Gaps** — `niriLayoutGapsOverrideEnabled` — Use custom gaps instead of bar spacing
- **Resize on Border** — `hyprlandResizeOnBorder` — Resize windows by dragging their edges with the mouse
- **Window Corner Radius** — `hyprlandLayoutRadiusOverride` — Rounded corners for windows
- **Window Corner Radius** — `mangoLayoutRadiusOverride` — Rounded corners for windows
- **Window Corner Radius** — `niriLayoutRadiusOverride` — Rounded corners for windows
- **Window Gaps** — `hyprlandLayoutGapsOverride` — Space between windows
- **Window Gaps** — `mangoLayoutGapsOverride` — Space between windows
- **Window Gaps** — `niriLayoutGapsOverride` — Space between windows

## Time & Weather  (16)

- **24-Hour Format** — `use24HourClock` — Use 24-hour time format instead of 12-hour AM/PM
- **Auto Location** — `useAutoLocation` — Automatically determine your location using your IP address
- **Calendar Backend** — `calendarBackend`
- **Date Format** — `dateFormat` — Show week number in the calendar
- **Enable Weather** — `weatherEnabled` — Show weather information in top bar and control center
- **First Day of Week** — `firstDayOfWeek`
- **Lock Screen Format** — `lockDateFormat` — Preview: %1
- **Pad Hours** — `padHours12Hour` — 02:31 PM vs 2:31 PM
- **Show Seconds** — `showSeconds` — Display seconds in the clock
- **Show Week Number** — `showWeekNumber` — Show week number in the calendar
- **Time & Weather** — `_tab_1`
- **Time Format** — `timeFormat` — Use 24-hour time format instead of 12-hour AM/PM
- **Top Bar Format** — `clockDateFormat` — Preview: %1
- **Use Imperial Units** — `useFahrenheit` — Use Imperial units (°F, mph, inHg) instead of Metric (°C, km/h, hPa)
- **Weather** — `weather` — Show weather information in top bar and control center
- **Wind Speed in m/s** — `windSpeedUnit` — Use meters per second instead of km/h for wind speed

## Keyboard Shortcuts  (1)

- **Keyboard Shortcuts** — `_tab_2`  _[keybindsAvailable]_

## Dank Bar  (38)

- **Bar Configurations** — `barConfigurations`
- **Display Assignment** — `barDisplay`
- **Position** — `barPosition`
- **Settings** — `_tab_3`
- **Use Overlay Layer** — `barUseOverlayLayer` — Place the bar on the Wayland overlay layer
- **Visibility** — `barVisibility` — Automatically hide the bar when the pointer moves away
- **Drag to Reorder** — `workspaceDragReorder` — Drag workspace indicators to reorder them  _[isNiri]_
- **Follow Monitor Focus** — `workspaceFollowFocus` — Show workspaces of the currently focused monitor  _[isNiri]_
- **Group Active Workspace** — `groupActiveWorkspaceApps` — Also group repeated application icons on the active workspace
- **Group Workspace Apps** — `groupWorkspaceApps` — Group repeated application icons in unfocused workspaces
- **Highlight Active Workspace App** — `workspaceActiveAppHighlightEnabled` — Highlight the currently focused app inside workspace indicators
- **Named Workspace Icons** — `workspaceIcons`
- **Reverse Scrolling Direction** — `reverseScrolling` — Reverse workspace switch direction when scrolling over the bar  _[isNiri]_
- **Show All Tags** — `dwlShowAllTags` — Show all 9 tags instead of only occupied tags  _[isMango]_
- **Show Occupied Workspaces Only** — `showOccupiedWorkspacesOnly` — Display only workspaces that contain windows  _[isNiri]_
- **Show Workspace Apps** — `showWorkspaceApps` — Display application icons in workspace indicators  _[isNiri]_
- **Workspace Index Numbers** — `showWorkspaceIndex` — Show workspace index numbers in the top bar workspace switcher
- **Workspace Names** — `showWorkspaceName` — Show workspace name on horizontal bars, and first letter on vertical bars
- **Workspace Padding** — `showWorkspacePadding` — Always show a minimum of 3 workspaces, even if fewer are available
- **Workspace Settings** — `workspaceSettings` — Show workspace index numbers in the top bar workspace switcher  _[isNiri]_
- **Workspaces** — `_tab_4`
- **Border** — `barBorder` — Theme color used for the border
- **Corners & Background** — `barCorners` — Remove corner rounding from the bar
- **Dank Bar** — `barAppearance`
- **Direction Source** — `barShadowDirectionSource` — Choose how this bar resolves shadow direction
- **Focused Border** — `workspaceFocusedBorderEnabled` — Show an outline ring around the focused workspace indicator
- **Focused Border** — `workspaceUnfocusedMonitorBorderEnabled` — Show an outline ring around the focused workspace indicator
- **Font Scale** — `barFontScale` — Scale DankBar font sizes independently
- **Icon Scale** — `barIconScale` — Scale DankBar icon sizes independently
- **Manual Direction** — `barShadowDirectionManual` — Use a fixed shadow direction for this bar
- **Opacity** — `barTransparency` — Controls opacity of the bar background
- **Separate Appearance for Unfocused Display(s)** — `workspaceUnfocusedMonitorSeparateAppearance` — Use different workspace colors on displays that are not focused
- **Shadow Override** — `barShadow` — Override the global shadow with per-bar settings
- **Spacing** — `barSpacing` — Space between the bar and screen edges
- **System Tray Icon Tint** — `trayIconTint` — Controls how much original icon color is removed before applying tint
- **Widget Outline** — `barWidgetOutline` — Theme color used for the widget outline
- **Workspace Appearance** — `workspaceAppearance` — Show an outline ring around the focused workspace indicator
- **Widgets** — `_tab_22`

## Dock  (29)

- **Auto-hide Dock** — `dockAutoHide` — Always hide the dock and reveal it when hovering near the dock area
- **Behavior** — `dockBehavior` — Only show windows from the current monitor on each dock  _[isHyprland]_
- **Border** — `dockBorder` — Add a border around the dock
- **Brightness** — `dockLauncherLogoBrightness`
- **Contrast** — `dockLauncherLogoContrast`
- **Dock & Launcher** — `_tab_5`
- **Dock Visibility** — `dockVisibility` — Display a dock with pinned and running applications
- **Group by App** — `dockGroupByApp` — Group multiple windows of the same app together with a window count indicator
- **Icon Size** — `dockIconSize`
- **Indicator Style** — `dockIndicatorStyle`
- **Intelligent Auto-hide** — `dockSmartAutoHide` — Show dock when floating windows don  _[isNiri]_
- **Isolate Displays** — `dockIsolateDisplays` — Only show windows from the current monitor on each dock
- **Launcher Button** — `dockLauncher`
- **Max Pinned Apps (0 = Unlimited)** — `dockMaxVisibleApps`
- **Max Running Apps (0 = Unlimited)** — `dockMaxVisibleRunningApps`
- **Opacity** — `dockTransparency`
- **Open Trash With** — `dockTrashFileManager` — File manager used to open the trash. Pick \
- **Position** — `dockPosition`
- **Restore Special Workspace Windows** — `dockRestoreSpecialWorkspaceOnClick` — When clicking a dock window in a Hyprland special workspace, bring that special workspace back before focusing the window  _[isHyprland]_
- **Show Dock** — `showDock` — Display a dock with pinned and running applications
- **Show Launcher Button** — `dockLauncherEnabled`
- **Show Overflow Badge Count** — `dockShowOverflowBadge` — Displays count when overflow is active
- **Show Trash in Dock** — `dockShowTrash` — Place a trash bin at the end of the dock
- **Show on Overview** — `dockOpenOnOverview` — Always show the dock when niri  _[isNiri]_
- **Size Offset** — `dockLauncherLogoSizeOffset`
- **Sizing** — `dockSizing`
- **Spacing** — `dockSpacing`
- **Trash** — `dockTrash` — Place a trash bin at the end of the dock
- **Use Overlay Layer** — `dockUseOverlayLayer` — Place the dock on the Wayland overlay layer

## Network  (6)

- **Network** — `_tab_7`  _[dmsConnected]_
- **Network Status** — `networkStatus`
- **Ethernet** — `networkEthernet`
- **Saved Networks** — `networkSavedWifi`
- **WiFi** — `networkWifi`
- **VPN** — `networkVpn`

## System  (12)

- **Printers** — `_tab_8`  _[cupsAvailable]_
- **Advanced** — `disabled` — Clipboard works but nothing saved to disk
- **Auto-Clear After** — `autoClearDays` — Automatically delete entries older than this
- **Behavior** — `clearAtStartup` — Clear all history when server starts
- **Click to Paste** — `clipboardClickToPaste` — Click an entry to paste directly instead of copying
- **Clipboard** — `_tab_23`  _[dmsConnected]_
- **Enter to Paste** — `clipboardEnterToPaste` — Press Enter to paste, Shift+Enter to copy
- **History Settings** — `maxHistory` — Maximum number of clipboard entries to keep
- **Maximum Entry Size** — `maxEntrySize` — Maximum size per clipboard entry
- **Maximum Pinned Entries** — `maxPinned` — Maximum number of entries that can be saved
- **Remember Type Filter** — `clipboardRememberTypeFilter` — Keep the clipboard type filter when reopening history
- **Visible Entry Actions** — `clipboardVisibleEntryActions` — Choose which action buttons appear on clipboard entries

## Launcher  (34)

- **App Customizations** — `appOverrides`
- **Appearance** — `dankLauncherV2Appearance` — Show mode tabs and keyboard hints at the bottom.
- **Border** — `dankLauncherV2BorderEnabled`
- **Brightness** — `launcherLogoBrightness`
- **Contrast** — `launcherLogoContrast`
- **DMS** — `builtInPlugins`
- **Default Launcher** — `launcherStyle` — Use the overlay layer when opening the launcher
- **Default Opens** — `launcherStyleSelector`
- **Enable Overview Overlay** — `niriOverviewOverlayEnabled` — Show launcher overlay when typing in Niri overview. Disable to use another launcher.
- **Grid Columns** — `appLauncherGridColumns` — Adjust the number of columns in grid view mode.
- **Hidden Apps** — `hiddenApps`
- **Include Files in All Tab** — `dankLauncherV2IncludeFilesInAll` — Merge indexed file results into the All tab (requires dsearch).
- **Include Folders in All Tab** — `dankLauncherV2IncludeFoldersInAll` — Merge indexed folder results into the All tab (requires dsearch).
- **Invert on mode change** — `launcherLogoColorInvertOnMode`
- **Launch Prefix** — `launchPrefix`
- **Launcher** — `_tab_9`
- **Launcher Button Logo** — `launcherLogo`
- **Niri Integration** — `spotlightCloseNiriOverview` — Auto-close Niri overview when launching apps.  _[isNiri]_
- **Plugin Visibility** — `pluginVisibility`
- **Recently Used Apps** — `recentApps`
- **Remember Last Mode** — `rememberLastMode` — Restore the last selected mode (tab) when the launcher is opened
- **Remember Last Query** — `rememberLastQuery` — Autofill last remembered query when opened
- **Search App Actions** — `searchAppActions` — Include desktop actions (shortcuts) in search results.
- **Search Options** — `searchOptions` — Include desktop actions (shortcuts) in search results.
- **Show Footer** — `dankLauncherV2ShowFooter` — Show mode tabs and keyboard hints at the bottom.
- **Show Mode Chips** — `spotlightBarShowModeChips` — Show All, Apps, Files, and Plugins chips beside the Spotlight Bar input.
- **Show Package Source Badges** — `dankLauncherV2ShowSourceBadges` — Show Flatpak, Snap, AppImage, or Nix badge icons on launcher items.
- **Size Offset** — `launcherLogoSizeOffset`
- **Sort Alphabetically** — `sortAppsAlphabetically` — When enabled, apps are sorted alphabetically. When disabled, apps are sorted by usage frequency.
- **Sorting & Layout** — `launcherSorting` — When enabled, apps are sorted alphabetically. When disabled, apps are sorted by usage frequency.
- **Spotlight Bar** — `spotlightBarLauncher` — Show All, Apps, Files, and Plugins chips beside the Spotlight Bar input.
- **Thickness** — `dankLauncherV2BorderThickness`
- **Unload on Close** — `dankLauncherV2UnloadOnClose` — Free VRAM/memory when the launcher is closed. May cause a slight delay when reopening.
- **Use Overlay Layer** — `launcherUseOverlayLayer` — Use the overlay layer when opening the launcher

## Theme & Colors  (71)

- **Alacritty** — `matugenTemplateAlacritty`
- **Applications** — `applications` — Sync dark mode with settings portals for system-wide theme hints
- **Auto-Hide Timeout** — `cursorHideAfterInactive` — Hide cursor after inactivity (0 = disabled)
- **Automatic Color Mode** — `automaticColorMode`
- **Background Blur** — `blurEnabled` — Your compositor does not support background blur (ext-background-effect-v1)
- **Bar Shadows** — `barElevationEnabled` — Shadow elevation on bars and panels
- **Blur Border Color** — `blurBorderColor` — Border color around blurred surfaces
- **Blur Border Opacity** — `blurBorderOpacity` — Controls the outer edge of protocol-blurred windows
- **Button Color** — `buttonColorMode` — Color for primary action buttons
- **Color Mode** — `colorMode` — Use light theme instead of dark theme
- **Control Center Tile Color** — `controlCenterTileColorMode` — Active tile background and icon color
- **Corner Radius** — `cornerRadius` — 0 = square corners
- **Cursor Size** — `cursorSize` — Mouse pointer size in pixels
- **Cursor Theme** — `cursorTheme` — Mouse pointer appearance  _[isNiri]_
- **Custom Blend** — `widgetBackgroundCustomStrength` — Blend between Surface High and the selected custom color
- **Dark Mode Icon Theme** — `iconThemeDark` — DankShell & System Icons (requires restart)
- **Dark mode base** — `matugenTemplateNeovimSettings` — Base to derive dark theme from
- **Darken Modal Background** — `modalDarkenBackground` — Show darkened overlay behind modal dialogs
- **Emacs** — `matugenTemplateEmacs`
- **Firefox** — `matugenTemplateFirefox`
- **Follow DMS background color** — `matugenTemplateNeovimSetBackground`
- **Foreground Layers** — `blurForegroundLayers` — Show foreground surfaces on panels for stronger contrast
- **GTK** — `matugenTemplateGtk`
- **Ghostty** — `matugenTemplateGhostty`
- **Hide When Typing** — `cursorHideWhenTyping` — Hide cursor when pressing keyboard keys  _[isNiri]_
- **Hide on Touch** — `cursorHideOnTouch` — Hide cursor when using touch input  _[isHyprland]_
- **Hyprland** — `matugenTemplateHyprland`
- **Icon Theme** — `iconTheme` — Use different icon themes for light and dark mode
- **KColorScheme** — `matugenTemplateKcolorscheme`
- **Layer Outline Opacity** — `blurLayerOutlineOpacity` — Controls outlines around foreground cards, pills, and notification cards
- **Light Direction** — `m3ElevationLightDirection` — Controls shadow cast direction for elevation layers
- **Light Mode** — `isLightMode` — Use light theme instead of dark theme
- **Light Mode Icon Theme** — `iconThemeLight` — DankShell & System Icons (requires restart)
- **Matugen Contrast** — `matugenContrast` — Adjusts contrast of generated colors (-100 = minimum, 0 = standard, 100 = maximum)
- **Matugen Palette** — `matugenScheme` — Select the palette algorithm used for wallpaper-based colors
- **Matugen Templates** — `matugenTemplates`  _[matugenAvailable]_
- **Modal Background** — `modalBackground` — Show darkened overlay behind modal dialogs
- **Modal Shadows** — `modalElevationEnabled` — Shadow elevation on modals and dialogs
- **Natural Touchpad Scrolling** — `mangoTrackpadNaturalScrolling` — Invert touchpad scroll direction  _[isMango]_
- **Popout Shadows** — `popoutElevationEnabled` — Shadow elevation on popouts, OSDs, and dropdowns
- **Run DMS Templates** — `runDmsMatugenTemplates`
- **Run User Templates** — `runUserMatugenTemplates`
- **Separate Light & Dark Themes** — `iconThemePerMode` — Use different icon themes for light and dark mode
- **Shadow Color** — `m3ElevationColorMode` — Base color for shadows (opacity is applied automatically)
- **Shadow Intensity** — `m3ElevationIntensity` — Controls the base blur radius and offset of shadows
- **Shadow Opacity** — `m3ElevationOpacity` — Controls the opacity of the shadow
- **Shadows** — `m3ElevationEnabled` — Material inspired shadows and elevation on modals, popouts, and dialogs
- **Surface Opacity** — `popupTransparency` — Controls opacity of shell surfaces, popouts, and modals
- **Sync Mode with Portal** — `syncModeWithPortal` — Sync dark mode with settings portals for system-wide theme hints
- **System App Theming** — `systemAppTheming`  _[matugenAvailable]_
- **Terminals - Always use Dark Theme** — `terminalsAlwaysDark` — Force terminal applications to always use dark color schemes
- **Theme & Colors** — `_tab_10`
- **Theme Color** — `themeColor` — Select the palette algorithm used for wallpaper-based colors
- **VS Code** — `matugenTemplateVscode`
- **WezTerm** — `matugenTemplateWezterm`
- **Widget Styling** — `widgetStyling` — Choose neutral or accent-colored widget text
- **Widget Text Style** — `widgetColorMode` — Choose neutral or accent-colored widget text
- **Zed** — `matugenTemplateZed`
- **dgop** — `matugenTemplateDgop`
- **equibop** — `matugenTemplateEquibop`
- **foot** — `matugenTemplateFoot`
- **kitty** — `matugenTemplateKitty`
- **mangowc** — `matugenTemplateMangowc`
- **neovim** — `matugenTemplateNeovim` — Required plugin:
- **niri** — `matugenTemplateNiri`
- **pywalfox** — `matugenTemplatePywalfox`
- **qt5ct** — `matugenTemplateQt5ct`
- **qt6ct** — `matugenTemplateQt6ct`
- **vencord** — `matugenTemplateVencord`
- **vesktop** — `matugenTemplateVesktop`
- **zenbrowser** — `matugenTemplateZenBrowser`

## Lock Screen  (23)

- **Active Lock Screen Monitor** — `lockScreenActiveMonitor`
- **Automatic Cycling** — `lockScreenVideoCycling` — Pick a different random video each time from the same folder
- **Enable Video Screensaver** — `lockScreenVideoEnabled` — Play a video when the screen locks.
- **Enable fingerprint authentication** — `enableFprint`
- **Enable loginctl lock integration** — `loginctlLockIntegration` — Bind lock screen to dbus signals from loginctl. Disable if using an external lock screen
- **Enable security key authentication** — `enableU2f`
- **Lock Screen** — `_tab_11`
- **Lock Screen Display** — `lockDisplay`
- **Lock Screen behaviour** — `lockBehavior` — Bind lock screen to dbus signals from loginctl. Disable if using an external lock screen
- **Lock Screen layout** — `lockLayout` — If the field is hidden, it will appear as soon as a key is pressed.
- **Lock at startup** — `lockAtStartup` — Automatically lock the screen when DMS starts
- **Lock before suspend** — `lockBeforeSuspend` — Automatically lock the screen when the system prepares to suspend
- **Notification Display** — `lockScreenNotificationMode` — Control what notification information is shown on the lock screen
- **Power off monitors on lock** — `lockScreenPowerOffMonitorsOnLock` — Turn off all displays immediately when the lock screen activates
- **Security key mode** — `u2fMode`
- **Show Media Player** — `lockScreenShowMediaPlayer`
- **Show Password Field** — `lockScreenShowPasswordField` — If the field is hidden, it will appear as soon as a key is pressed.
- **Show Power Actions** — `lockScreenShowPowerActions`
- **Show Profile Image** — `lockScreenShowProfileImage`
- **Show System Date** — `lockScreenShowDate`
- **Show System Icons** — `lockScreenShowSystemIcons`
- **Show System Time** — `lockScreenShowTime`
- **Video Screensaver** — `videoScreensaver` — Play a video when the screen locks.

## Plugins  (1)

- **Plugins** — `_tab_12`

## About  (1)

- **About** — `_tab_13`

## Typography & Motion  (17)

- **%1 Animation Speed** — `modalAnimationSpeed` — %1 custom animation duration
- **%1 Animation Speed** — `popoutAnimationSpeed` — %1 custom animation duration
- **Animation Duration** — `customAnimationDuration` — Globally scale all animation durations
- **Animation Speed** — `animationSpeed` — Globally scale all animation durations
- **Animation Style** — `animationVariant`
- **Custom Duration** — `modalCustomAnimationDuration` — %1 custom animation duration
- **Custom Duration** — `popoutCustomAnimationDuration` — %1 custom animation duration
- **Font Scale** — `fontScale` — Scale all font sizes throughout the shell
- **Font Weight** — `fontWeight` — Select font weight for UI text
- **Monospace Font** — `monoFontFamily` — Select monospace font for process list and technical displays
- **Motion Effects** — `motionEffect`
- **Normal Font** — `fontFamily` — Select the font family for UI text
- **Ripple Effects** — `enableRippleEffects` — Show Material Design ripple animations on interactive elements
- **Sync Popouts & Modals** — `syncComponentAnimationSpeeds` — Popouts and Modals follow global Animation Speed (disable to customize independently)
- **Text Rendering** — `textRenderType`
- **Typography** — `typography` — Select the font family for UI text
- **Typography & Motion** — `_tab_14`

## Sounds  (10)

- **Enable System Sounds** — `soundsEnabled` — Play sounds for system events
- **Login** — `soundLogin` — Play sound after logging in
- **Mute During Playback** — `muteSoundsWhenMediaPlaying` — Silence system sounds while media is playing
- **New Notification** — `soundNewNotification` — Play sound when new notification arrives
- **Plugged In** — `soundPluggedIn` — Play sound when power cable is connected
- **Sound Theme** — `soundTheme` — Select system sound theme
- **Sounds** — `_tab_15`  _[soundsAvailable]_
- **System Sounds** — `systemSounds` — Play sounds for system events  _[soundsAvailable]_
- **Use System Theme** — `useSystemSoundTheme` — Use sound theme from system settings
- **Volume Changed** — `soundVolumeChanged` — Play sound when volume is adjusted

## Media Player  (4)

- **Excluded Media Players** — `mediaExcludePlayers`
- **Media Player** — `_tab_16`
- **Media Player Settings** — `mediaPlayer` — Use animated wave progress bars for media playback
- **Scroll Wheel** — `audioScrollMode` — Scroll wheel behavior on media widget

## Notifications  (28)

- **Body Font Size** — `notificationBodyFontSize` — Set the font size for notification body text (htmlBody)
- **Compact** — `notificationCompactMode` — Use smaller notification cards
- **Critical Priority** — `notificationHistorySaveCritical` — Save critical priority notifications to history
- **Critical Priority** — `notificationTimeoutCritical` — Timeout for critical priority notifications
- **Do Not Disturb** — `doNotDisturb` — Suppress notification popups while enabled
- **Duration** — `notificationCustomAnimationDuration` — Base duration for animations (drag to use Custom)
- **Enable History** — `notificationHistoryEnabled` — Save dismissed notifications to history
- **Focused Monitor Only** — `notificationFocusedMonitor` — Show notification popups only on the currently focused monitor
- **History Retention** — `notificationHistoryMaxAgeDays` — Auto-delete notifications older than this
- **History Settings** — `notificationHistory` — Save dismissed notifications to history
- **Lock Screen** — `lockScreenNotifications` — Control what notification information is shown on the lock screen
- **Low Priority** — `notificationHistorySaveLow` — Save low priority notifications to history
- **Low Priority** — `notificationTimeoutLow` — Timeout for low priority notifications
- **Maximum History** — `notificationHistoryMaxCount` — Maximum number of notifications to keep
- **Muted Apps** — `mutedApps`
- **Normal Priority** — `notificationHistorySaveNormal` — Save normal priority notifications to history
- **Normal Priority** — `notificationTimeoutNormal` — Timeout for normal priority notifications
- **Notification Overlay** — `notificationOverlayEnabled` — Display all priorities over fullscreen apps
- **Notification Popups** — `notificationPopups` — Set the font size for notification summary text
- **Notification Rules** — `notificationRules`
- **Notification Timeouts** — `notificationTimeouts` — Timeout for low priority notifications
- **Notifications** — `_tab_17`
- **Popup Position** — `notificationPopupPosition` — Choose where notification popups appear on screen
- **Popup Shadow** — `notificationPopupShadowEnabled` — Show drop shadow on notification popups. Requires M3 Elevation to be enabled in Theme & Colors.
- **Privacy Mode** — `notificationPopupPrivacyMode` — Hide notification content until expanded; popups show collapsed by default
- **Summary Font Size** — `notificationSummaryFontSize` — Set the font size for notification summary text
- **Suppress Duplicate Notifications** — `notificationDedupeEnabled` — Identical alerts show as one popup instead of stacking
- **Timeout Progress Bar** — `notificationShowTimeoutBar` — Show a bar that drains as the popup

## On-screen Displays  (12)

- **Always Show Percentage** — `osdAlwaysShowValue` — Display volume and brightness percentage values in OSD popups
- **Audio Output Switch** — `osdAudioOutputEnabled` — Show on-screen display when cycling audio output devices
- **Brightness** — `osdBrightnessEnabled` — Show on-screen display when brightness changes
- **Caps Lock** — `osdCapsLockEnabled` — Show on-screen display when caps lock state changes
- **Idle Inhibitor** — `osdIdleInhibitorEnabled` — Show on-screen display when idle inhibitor state changes
- **Media Playback** — `osdMediaPlaybackEnabled` — Show on-screen display when media player status changes
- **Media Volume** — `osdMediaVolumeEnabled` — Show on-screen display when media player volume changes
- **Microphone Mute** — `osdMicMuteEnabled` — Show on-screen display when microphone is muted/unmuted
- **OSD Position** — `osdPosition` — Choose where on-screen displays appear on screen
- **On-screen Displays** — `osd` — Choose where on-screen displays appear on screen
- **Power Profile** — `osdPowerProfileEnabled` — Show on-screen display when power profile changes
- **Volume** — `osdVolumeEnabled` — Show on-screen display when volume changes

## Running Apps  (2)

- **App ID Substitutions** — `appIdSubstitutions`
- **Running Apps** — `_tab_19`  _[isHyprlandOrNiri]_

## System Updater  (2)

- **Advanced** — `systemUpdaterAdvanced` — Open a terminal and run a custom command instead of the in-shell upgrade flow.
- **System Updater** — `systemUpdater` — How often the server polls for new updates.

## Power & Sleep  (18)

- **Automatically lock after** — `lockTimeout`
- **Custom Power Actions** — `customPowerActions`
- **Default selected action** — `powerMenuDefaultAction`
- **Fade to lock screen** — `fadeToLockEnabled` — Gradually fade the screen before locking with a configurable grace period
- **Fade to monitor off** — `fadeToDpmsEnabled` — Gradually fade the screen before turning off monitors with a configurable grace period
- **Hold Duration** — `powerActionHoldDuration`
- **Hold to Confirm Power Actions** — `powerActionConfirm` — Require holding button/key to confirm power off, restart, suspend, hibernate and logout
- **Idle Settings** — `idleSettings` — Gradually fade the screen before locking with a configurable grace period
- **Lock fade grace period** — `fadeToLockGracePeriod`
- **Monitor fade grace period** — `fadeToDpmsGracePeriod`
- **Power & Sleep** — `_tab_21`
- **Power Action Confirmation** — `powerConfirmation` — Require holding button/key to confirm power off, restart, suspend, hibernate and logout
- **Power Menu Customization** — `powerMenu` — Display power menu actions in a grid instead of a list
- **Suspend system after** — `suspendTimeout`
- **Switch to power profile** — `powerProfile`
- **Turn off monitors after** — `monitorTimeout`
- **Turn off monitors after lock** — `postLockMonitorTimeout`
- **Use Grid Layout** — `powerMenuGridLayout` — Display power menu actions in a grid instead of a list

## Displays  (5)

- **Displays** — `_tab_24`
- **Day Temperature** — `nightModeHighTemperature` — Color temperature for day time
- **Gamma Control** — `_tab_25`
- **Night Temperature** — `nightModeTemperature` — Color temperature for night mode
- **Widgets** — `_tab_26`  _[dmsConnected]_

## Desktop Widgets  (1)

- **Desktop Widgets** — `_tab_27`

## Audio  (3)

- **Input Devices** — `audioInputDevices`
- **Output Devices** — `audioOutputDevices`
- **System** — `_tab_29`

## Locale  (3)

- **Locale** — `_tab_30`
- **Locale Settings** — `locale` — Change the locale used by the DMS interface.
- **Time & Date Locale** — `timeLocale` — Change the locale used for date and time formatting, independent of the interface language.

## Greeter  (17)

- **24-hour clock** — `greeterUse24Hour` — Greeter only — does not affect main clock
- **Auto-login on startup** — `greeterAutoLogin` — Skip the greeter password after boot until you sign out. Lock screen unlock is unchanged. Takes effect on the next reboot after sync.
- **Date Format** — `greeterLockDateFormat` — Greeter only — format for the date on the login screen
- **Dependencies & documentation** — `greeterDeps`
- **Enable fingerprint at login** — `greeterEnableFprint`
- **Enable security key at login** — `greeterEnableU2f`
- **Greeter** — `_tab_31`
- **Greeter Appearance** — `greeterAppearance` — Font used on the login screen
- **Greeter Behavior** — `greeterBehavior` — Pre-select the last used session on the greeter
- **Greeter Status** — `greeterStatus`
- **Greeter font** — `greeterFontFamily` — Font used on the login screen
- **Login Authentication** — `greeterAuth`
- **Pad hours (02:00 vs 2:00)** — `greeterPadHours`
- **Remember last session** — `greeterRememberLastSession` — Pre-select the last used session on the greeter
- **Remember last user** — `greeterRememberLastUser` — Pre-fill the last successful username on the greeter
- **Show Seconds** — `greeterShowSeconds`
- **Wallpaper fill mode** — `greeterWallpaperFillMode` — How the background image is scaled

## Multiplexers  (3)

- **Multiplexer** — `muxType` — Terminal multiplexer backend to use
- **Multiplexers** — `_tab_32`
- **Terminal** — `muxUseCustomCommand` — Override terminal with a custom command or script

## Frame  (20)

- **Arc Extender** — `frameLauncherArcExtender` — Use the extended surface for launcher content
- **Bar Inset Padding** — `frameBarInsetPadding` — Gap between the end widgets and the bar ends (0 = edge-to-edge)
- **Border** — `frameBorder` — Horizontal and vertical bar thickness
- **Border Color** — `frameColor`
- **Border Radius** — `frameRounding`
- **Border Width** — `frameThickness`
- **Connected Options** — `frameConnectedOptions` — Reveal the arcs where surfaces meet the frame
- **Display Assignment** — `frameDisplays`
- **Edge Hover Reveal** — `frameLauncherEdgeHover` — Open the launcher by hovering the emerge edge (when free of bar and dock)
- **Enable Frame** — `frameEnable` — Draw a connected picture-frame border around the entire display
- **Expose the Arcs** — `frameCloseGaps` — Reveal the arcs where surfaces meet the frame
- **Frame** — `frameEnabled` — Draw a connected picture-frame border around the entire display
- **Frame Blur** — `frameBlurEnabled` — Requires a newer version of Quickshell
- **Integrations** — `frameBarIntegration` — Show during Niri overview  _[isNiri]_
- **Launcher Emerge Side** — `frameLauncherEmergeSide` — Edge the launcher slides from
- **Mode** — `frameMode` — Surfaces emerge flush from the bar
- **Show on Overview** — `frameShowOnOverview` — Show during Niri overview
- **Size** — `frameBarSize` — Horizontal and vertical bar thickness
- **Surface Behavior** — `frameModeSelector` — Surfaces emerge flush from the bar
- **Surface Opacity** — `frameOpacity`

## Default Apps  (1)

- **Applications** — `_tab_34`

## Users  (5)

- **Allow greeter login access** — `createUserGreeter` — Add the new user to the %1 group so they can run dms greeter sync --profile.
- **Create User** — `createUser` — Add the new user to the %1 group so they can use sudo.
- **Existing Users** — `usersList`
- **Grant administrator privileges** — `createUserAdmin` — Add the new user to the %1 group so they can use sudo.
- **Users** — `_tab_35`

## Autostart  (2)

- **Autostart Apps** — `_tab_36`
- **Autostart Entries** — `autostartEntries`

## Applications  (1)

- **Window Rules** — `windowRules` — Define compositor rules for window behavior  _[windowRulesCapable]_

## Settings  (1)

- **Power & Security** — `_tab_42`

## Dank Dash  (1)

- **Widgets & Notifications** — `_tab_43`
