# DankC — Polish & Gaps Backlog (toward DMS parity + max performance)

Feature-complete for v1 (see docs/TASKS.md: 36/37 done, only plugins deferred).
This file is the **quality** backlog: what's missing/rough vs DankMaterialShell
(DMS) in UI/UX, Material fidelity, and performance. Work top-down within each
group; each item says where in the code to look.

Reference DMS live (it's running): read its QML at
~/Downloads/DankMaterialShell-master/quickshell/ and the design docs in
~/dankc/docs/10-DESIGN-SYSTEM.md + 11-UX-FLOW.md (exact tokens/shaders/durations).

## P1 — Material fidelity (biggest visual gap)
- **Elevation shadows**: DMS uses a GLSL elevation shader (docs/10 §elevation).
  We use nvgBoxGradient — close but not DMS. Port the real shadow ramp / radii
  per elevation level (1–5). Files: all ui/*.c shadow blocks.
- **Corner radius + spacing tokens**: adopt DMS tokens exactly (spacing
  4/8/12/16/24; cornerRadius 12; icon 24/16/32; barHeight 48). Audit every
  hardcoded px against docs/10. Files: ui/bar/bar.c, ui/*.c.
- **Dynamic color = true HCT/MCU**: theme/dynamic.cpp is an HSL approximation.
  Port Material Color Utilities (HCT + tonal palettes + Celebi quantization) so
  wallpaper colors match matugen exactly. Add LIGHT scheme variants.
- **Fonts**: confirm Inter weight ramp (400/500/600) + FiraCode for mono; DMS
  font sizes 12/14/16/20. render/nvg.c currently loads one Inter face.

## P2 — Material background + frame (DMS signature look)
- **Blurred wallpaper "material" background** behind panels (DMS BlurService,
  3-layer, wp_* shaders, 1000ms InOutCubic transitions). Panels are currently
  transparent-over-wallpaper; add the blur layer. docs/11 §material bg.
- **Rounded screen corners** (DMS Frame): overlay layer drawing screen-corner
  radius. New ui/frame.c.

## P3 — Bar polish — DONE 2026-07-02 (S1–S6, see docs/12-BAR-SPEC.md)
- ~~Hover states~~ ✅ (tooltips: DMS bar pills mostly have none — matched behavior).
- ~~Workspace pill morph animation~~ ✅ frame-callback driven.
- Click feedback: ripple OFF in user's DMS config — intentionally not implemented.
- ~~Media widget~~ ✅ (transport inline; marquee + bar album-art N/A per DMS anatomy).
- ~~Battery~~ ✅ glyph + AC bolt (time-to-empty deferred to battery popout, docs/13).
- ~~Per-widget spacing/order~~ ✅ config-driven from user's DMS arrays.

## P4 — Panels: depth to match DMS
- **Control Center**: expandable sections, live % labels on sliders, network
  list (pick SSID), bluetooth device list, media controls, power profiles,
  wire Dark/Night to real actions (gsettings/gammastep). Files: ui/controlcenter.c.
- **Notification Center**: grouping by app, action buttons, inline images,
  scroll for long history, swipe-to-dismiss. Files: ui/notifcenter.c, services/notifications.c.
- **Launcher**: categories, recent/frequent apps, calculator/math, run actions,
  mouse hover selection, keyboard scroll. Files: ui/launcher.c.
- **Settings**: full DMS parity (424 options, tabbed) — see docs/09-DMS-SETTINGS-
  INVENTORY.md. Currently a subset. Files: ui/settings.c.
- **Clipboard**: pinned entries, image entries (IconPixmap), delete-entry.
- **Tray**: click-to-activate (Activate/ContextMenu), IconPixmap (ARGB) items,
  menu popups. Files: services/tray.c, ui/bar/bar.c draw_tray.

## P5 — Missing surfaces DMS has
- **Dock** (auto-hide app dock, 225ms) — new ui/dock.c.
- **Media/MPRIS popout** with album art + scrubber.
- **Power menu** (logout/reboot/shutdown/suspend/lock) — logind + a modal.
- **Brightness OSD** (reuse ui/osd.c; trigger on brightness change).
- **Weather + system-monitor widgets** (cpu/ram/temp from /proc, /sys).
- **Workspace overview** (optional).

## P6 — Animation completeness
- Workspace pill, per-card toast in/out, hover micro-animations, modal scrim
  fade, exit anims already done for popouts/OSD. Engine is core/anim.c
  (durations+easing done). Mostly needs frame-callback driving on the bar.

## P7 — Performance — DONE 2026-07-03 (wt/perf, items 1-4 below)
- ~~**Partial redraw / damage tracking**~~ ✅ `dc_bar_render()` (ui/bar/bar.c)
  now hashes everything that can change the bar's pixels (clock text,
  workspaces/focused window, media, weather, cpu/mem, battery, controlCenter's
  net/bluetooth/audio state, notification unread dot, tray, hover) before
  touching EGL/GL and skips the whole frame when it's unchanged and no
  frame-callback animation (workspace morph / media marquee) is in flight.
  Measured (2-output idle bar, 60s window): 117 tick calls -> only 24-34
  actual GL frames drawn (66-79% skipped); the rest were legitimate
  cpu/mem-widget redraws from background load on the dev machine during
  measurement, not idle noise. `DANKC_RENDER_STATS=1` logs drawn/skipped
  counts every 60s for future verification.
- ~~**Reduce polling**~~ ✅ audio.c's wpctl cache widened from an effective
  ~1s to a real 3s window (measured: 45 -> 14 wpctl forks per 32s, ~69%
  fewer) — libpipewire stays out of scope per this pass. net.c's sysfs
  link-state scan got a small 2s cache too (its nmcli-backed SSID lookup was
  already 3s-cached). bluez.c (3s), sysmon.c (3s cpu/mem, 2s process-scan
  gated behind the Processes popout), and weather.c (15min/2min retry) were
  already well-paced — no changes needed. Trade-off: external volume-change
  detection latency is now up to 3s worst-case (own-slider/OSD writes still
  invalidate the cache immediately, so those stay instant).
- ~~**Font subsetting**~~ ✅ assets/fonts/MaterialSymbolsRounded.ttf (14.5MB
  full variable font) is pyftsubset'd to the exact 73 codepoints
  render/icons.h's DC_ICON_* actually use ->
  MaterialSymbolsRounded.subset.ttf (244KB, ~98% smaller), which is what
  render/nvg.c now loads and meson.build now installs; the full font stays
  in the repo only as the regeneration source for scripts/subset-fonts.sh
  (re-run it whenever a new DC_ICON_ is added). Verified via screenshot: every
  icon in the bar and the Control Center popout renders correctly, no tofu.
- **Lazy EGL** for panels: already create-on-show (unchanged this pass; not
  re-verified beyond the idle-bar RSS/CPU numbers below).
- ~~**Measure**~~ ✅ final combined numbers (2-output idle bar, no panels,
  live niri session): RSS 173920KB (~170MB, actually *below* the earlier
  ~188MB pre-P7 measurement — mostly the font subset's smaller glyph-cache
  footprint), Pss 77MB / Private 39MB via smaps_rollup. Idle CPU stayed in
  the 0.2-0.3% band before and after (already near the measurement noise
  floor at this sample size on an otherwise-idle-ish dev machine). For
  contrast, the user's live DMS `qs` process measured Pss 595MB / Private
  521MB / VmRSS 803MB at the same time (read-only comparison, not touched).

## How to verify UI against DMS
1. Run only dankc (stop DMS) to avoid two top-bars fighting the layer:
   see README; then `grim` captures are unambiguous.
2. Compare regions side-by-side with DMS screenshots (python raw-PNG crop
   helper pattern used in the session; grim + downscale <2000px to view).
3. Every panel opens via `dankc ctl <launcher|control-center|notifications|
   clipboard|settings|lock>` — screenshot each and diff against DMS.
