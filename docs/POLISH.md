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

## P3 — Bar polish
- **Hover states + tooltips** on every bar item (DMS highlights on hover with a
  rounded bg; tooltips on icons). Needs pointer-motion tracking -> re-render.
- **Workspace pill morph animation** on switch (width/position tween; needs bar
  frame-callback driving, currently 1 Hz tick). Files: ui/bar/bar.c draw_workspaces.
- **Click feedback** (ripple/press) on bar buttons.
- **Media widget**: album art + play/pause/next inline (services/mpris.c already
  reads title/artist; add art + controls).
- **Battery**: charging bolt overlay + time-to-empty (UPower via D-Bus).
- **Per-widget exact spacing/order** vs DMS right cluster (compare with grim).

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

## P7 — Performance
- **Partial redraw / damage tracking**: bar re-renders fully every 1 Hz. Only
  redraw on real change; clock could tick per-minute + on second only if seconds
  shown. Files: ui/bar/bar.c, main.c clock_tick.
- **Reduce polling**: audio (wpctl fork/sec) -> libpipewire or longer cache;
  bluez/net cache tuning. Files: services/audio.c, net.c, bluez.c.
- **Lazy EGL** for panels (already create-on-show). Verify no idle GPU work.
- **Measure**: RSS ~145MB (Mesa-heavy) vs DMS 477MB; profile GPU/CPU with the
  panels open; target <1% idle CPU, no wakeups when nothing changes.

## How to verify UI against DMS
1. Run only dankc (stop DMS) to avoid two top-bars fighting the layer:
   see README; then `grim` captures are unambiguous.
2. Compare regions side-by-side with DMS screenshots (python raw-PNG crop
   helper pattern used in the session; grim + downscale <2000px to view).
3. Every panel opens via `dankc ctl <launcher|control-center|notifications|
   clipboard|settings|lock>` — screenshot each and diff against DMS.
