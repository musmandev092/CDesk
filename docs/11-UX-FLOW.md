# DankC — Shell Composition & Interaction Flow (extracted from DMS)

How surfaces layer and how everything opens/closes/animates. Every visual element in DMS is its **own
wlr-layer-shell surface** (`PanelWindow` = `zwlr_layer_surface_v1`), not one compositing window — so DankC
creates one layer surface per element. Z-order = (1) wlr layer enum, then (2) creation order within a layer.

## 1. Surface z-order (bottom → top)

| Element | wlr layer | namespace | exclusive zone | notes |
|---|---|---|---|---|
| Blurred wallpaper ("material" bg) | Background | `dms:blurwallpaper` | ignore | only if `blurredWallpaperLayer` + niri |
| Wallpaper | Background | (default) | ignore | click-through (empty input mask) |
| Desktop widgets | Bottom (or Overlay) | `dms:desktop-widget:<plugin>:<inst>` | ignore | per-plugin |
| Bar | **Top** (or Overlay via env/config) | `dms:bar` | **reserves** thickness | |
| Frame (connected chrome owner) | Top | `dms:frame` | — | draws joined corners for connected mode |
| Dock | Top (or Overlay) | `dms:dock` (+`:dock-exclusion`) | conditional | |
| Popouts | Top (Overlay if trigger on overlay) | `dms:popout` (+`:background`) | -1 | singleton, re-parented per screen |
| Modals | Top (Overlay if configured) | `dms:*` (+`:clickcatcher`) | -1 | scrim + content |
| Notification popups | Top (Overlay if enabled or Critical) | `dms:notification-popup` | -1 | per-monitor manager |
| OSDs | **Overlay** | `dms:osd` | -1, kbd None | |
| Fade-to-lock / DPMS | Overlay | `dms:fade-to-lock` / `dms:fade-to-dpms` | — | |
| Lock screen | **session-lock** (above all) | — | — | ext-session-lock-v1 |

Env overrides (`DMS_{DANKBAR,POPOUT,MODAL,OSD,NOTIFICATION}_LAYER` = background/bottom/top/overlay) via a
`LayerShell.fromEnv(name, fallback, allowlist)` helper — replicate as a config/env-driven layer selector.

**Per-monitor** = one surface per output, filtered by `getFilteredScreens("<role>")` (wallpaper/dock/osd/
notifications each have their own role filter). **Popouts/modals are global singletons** re-positioned to
the triggering screen; **only one popout open at a time across all monitors**.

**Surface recovery (must replicate):** screen hotplug / DPMS off→on / resume orphans layer surfaces. DMS
debounces (450 ms) then runs 2 recovery passes (800 ms, 2000 ms, 4000 ms cooldown) toggling
`barSurfacesLoaded`/`dockEnabled`/etc off→on to force re-map; wallpaper does a `visible false→true` bounce.
DankC: on output add/remove, tear down and rebuild affected layer surfaces after a short debounce.

## 2. Wallpaper / blur "material" background

Three stacked Background-layer surfaces per monitor:
1. **Blurred wallpaper** (the frosted "material" base): same image → blur (0.8, max 75) → sits at bottom so
   translucent widgets show a blurred wallpaper. Cross-fade transition 1000 ms `InOutCubic`.
2. **Wallpaper**: transparent surface, click-through; two `Image`s (`current`/`next`) into shader sources.
   Per-monitor path + fill mode. **Render is paused** unless a transition/scroll/load is active
   (`updatesEnabled = renderActive || settleFrames>0`, 3-frame settle) — replicate to avoid pinning the GPU.
3. Fallback solid `backgroundColor` + `DankBackdrop` when no image.

**Transition flow:** source change → pick type (`random` picks from included set, per-type random params:
wipe dir, disc/portal center, stripe count/angle) → load `next` → on Ready, enable shader sources,
**16 ms pre-roll**, then `progress` 0→1 over **1000 ms InOutCubic** → promote next→current, run queued.
Shaders per type map to the `wp_*` set (`10-DESIGN-SYSTEM §3`). "Scrolling" fill = separate parallax
pipeline driven by a damped-harmonic spring (niri scrolls Y).

**BlurService** (separate): detects `ext-background-effect-v1` support (`dms blur check`); exposes
`enabled`/`borderColor`/`borderWidth`. Frosted panels **request a blur region from the compositor**
(`WindowBlur`) rather than blurring pixels themselves + draw a 1px material border. DankC: if niri exposes
`ext-background-effect`, request backdrop blur; else our own dual-Kawase (off by default).

## 3. Popout / Modal / OSD flow

**Managers (singletons):**
- **PopoutManager** — one popout open globally. Opening closes others + all modals. Same-trigger click
  closes; clicking a hover-opened (transient) popout **pins** it (disables hover-dismiss); opening on
  another screen migrates it. Hover geometry via `cursorOverBar()` + `containsGlobalPoint()`.
- **ModalManager** — closes other modals (unless `allowStacking`) + all popouts (unless `keepPopoutsOpen`)
  + tray menus.
- **OSDManager** — one OSD per screen; new hides previous.
- **PopoutService** — public API (`open/close/toggle/unload<X>`, `setPosition`), holds lazy-loader refs +
  pending-open flags so IPC can request a popout before it's instantiated.

**Popout open (the "morph"):** full-screen transparent surface; content rect positioned relative to the
trigger bar widget. Positioning by bar edge: Top → y just below bar; Bottom → y = triggerY − height;
centered axis clamped to `[edgeGap, screenW−W−edgeGap]`; gap = `max(4, barSpacing)` snapped to device px
using the **real fractional scale**. Animation `openProgress` 0→1 drives simultaneously:
`scale = collapsed + (1−collapsed)·p` (from `effectScaleCollapsed`), `offset·(1−p)` slide from the bar edge,
opacity 0→1. Duration = `popoutAnimationDuration` (**default 150 ms**), BezierSpline enter/exit curves.
Three effect families (config): **default** (scale+small offset), **directional** (slide full size,
clip-reveal), **depth** (small travel + scale).

**Popout close:** reverse morph, `shouldBeVisible=false`, close timer = `variantCloseInterval`, then hide +
emit closed.

**Dismissal:** (a) **click-outside** — full-screen input surface whose input mask = dismiss region *minus*
the content hole *minus* the bar (so bar clicks still route to the bar); (b) **Escape** — focus-helper Item
Keys handler (unless content handles keys); (c) **hover-leave** — a hover-body tracker closes transient
(unpinned) popouts. **Keyboard focus:** `Exclusive` normally, `None` during screenshot; on Hyprland
`OnDemand` + focus-grab whitelisting the bar (niri doesn't need this).

**Modal:** two surfaces — a `:clickcatcher` full-screen scrim (kbd None) + centered content. Same morph
(`animationType` scale default or slide), duration `modalAnimationDuration` (default 150 ms). Dimming scrim
opacity 0→`backgroundOpacity`.

**OSD:** single Overlay surface, kbd None, positioned by `osdPosition` (9 anchors) with bar+dock margin
offsets. `show()`: skip if suppressed; if visible just restart hide timer; else register + fade in. Visual:
opacity 0→1, scale 0.9→1, `mediumDuration` (**450 ms** at Short), `OutQuart`. Auto-hide **2000 ms** (hover
pauses it). Hide: reverse + close timer = duration+50.

## 4. Connected-surface system (multi-monitor "connected chrome")

**Purpose:** in connected-frame mode a single **frame surface** (`dms:frame`) draws a continuous border
around the screen, and the bar/dock/popouts/modals/notifications **fuse into it** (shared rounded corners,
no gaps) instead of floating as separate rounded panels. Since those are independent layer surfaces, the
frame must draw the joins — so they publish where their bodies are.

**Mechanism = a geometry/ownership bus:**
- `ConnectedModeState` — per-screen registry keyed by slot (`popout/modal/dock/notification`); each
  descriptor = `{bodyRect, animationOffset, barSide, phase, omitStart/EndConnector, ownerId}` + revisions,
  pruned on screen change.
- `ConnectedSurfaceLease` — single-owner-per-(screen,slot) arbiter; `beginClaim()` mints a unique id;
  `publish()` only when PopoutManager says this instance owns that screen; recovery hooks re-publish after
  screen churn.
- **Producers** publish live geometry: popouts (+ a **morph-travel** where switching between connected
  popouts slides/reshapes the body continuously between them), dock (publishes slide offset; auto-retracts
  when a modal claims its edge), notifications (union rect of visible cards).
- **Consumer** = the frame surface reads descriptors and draws joined corners via `connected_chrome` /
  `connected_arc` shaders.

**DankC decision:** this is the seamless-frame aesthetic. It's optional — if we don't ship connected-frame
mode in v1, popouts use the standalone path (independent rounded panels) and this whole subsystem is
skipped. Recommend: **v1 = standalone rounded panels**; connected-frame is a later enhancement (M6+), built
as a small in-process geometry bus (each surface reports `{screen,slot,bodyRect,animOffset,barSide,phase}`
with a per-slot lock; the frame surface subscribes and redraws).

## 5. Dock behavior

Per-monitor (`getFilteredScreens("dock")`), layer Top (or Overlay), `dms:dock` (+ `:dock-exclusion` window
reserving space when frame-managed). Position `dockPosition` (edge-anchored, vertical for Left/Right).
Exclusive zone when shown & not auto-hidden. **Reveal:** false if modal-retract; true on niri overview (if
enabled); smart auto-hide shows when no window overlaps the dock edge on the active workspace, else reveal
on hover/context-menu/sticky; plain auto-hide = hover only; `revealHold` 250 ms sticky after unhover.
**Animation:** a Translate pushed off-edge by `thickness+10` when hidden; `shortDuration` **225 ms**
`OutCubic` (or connected `variantDuration(popoutAnimationDuration)`). Tooltips after 250 ms. Apps from
`DockApps` (pinned + running, optional group-by-app); per-corner-radius background (flat on bar-adjacent
edge in connected mode), optional border, compositor `WindowBlur` when enabled.

## 6. Notification popup flow

Per-monitor `NotificationPopupManager` holds `popupWindows[]`, each a `dms:notification-popup` surface (Top,
Overlay if enabled or Critical, kbd None). Position `notificationPopupPosition` (Top/Bottom/±Center); card
width `min(400, max(320, screenW·0.23))`. **Stacking:** newest-first, each `screenY = runningY += cardH +
popupSpacing`; **hovered cards pin** their slot, others flow around. **Enter:** after `enterDelay` 160 ms,
slide `tx` from `±entryTravel`→0 over `notificationEnterDuration` (**~175 ms** = 0.875×base preset), enter
bezier; stack shifts animate `shortDuration` + standardDecel. **Exit:** slide to `±exitTravel` + opacity→0 +
scale→collapsed over `notificationExitDuration` (**~150 ms** = 0.75×base); 600 ms watchdog force-finalizes.
**Inline expand/collapse:** 185/150 ms. **Timeout:** default 5000 ms bar drains, frozen on hover. **Swipe
dismiss:** DragHandler on x; past `width·0.35` animate to `±width` `OutCubic`. A 500 ms sweeper reaps zombie
windows. All scale with `notificationAnimationSpeed` (base `[0,200,400,600]`; 0 = snap).

## 7. Duration quick-reference (default "Short" speed)

| Interaction | Duration / curve |
|---|---|
| Popout / modal open-close | 150 ms, BezierSpline enter/exit |
| OSD fade+scale | 450 ms OutQuart; auto-hide 2000 ms |
| Dock slide | 225 ms OutCubic; reveal-hold 250 ms |
| Wallpaper transition | 1000 ms InOutCubic; 16 ms pre-roll |
| Notification enter/exit | ~175 / ~150 ms; enter delay 160 ms; timeout 5000 ms |
| Ripple | `spatialDefault` (~500 ms base), hold to 60% then fade |
| Surface recovery | debounce 450 ms; passes 800→2000 ms; cooldown 4000 ms |

Base speed table + curves: `10-DESIGN-SYSTEM §1`. These are the exact motion values that make DankC *feel*
like Dank; bake them into `render/anim.c`.
