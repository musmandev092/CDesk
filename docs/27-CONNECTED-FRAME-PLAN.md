# dankc Connected-Frame Chrome — Plan (2026-07-04)

Per-surface stitching (NOT DMS's centralized shader). Each popout draws itself; make its bar-facing edge
merge into the bar. frame.c today = only 4 black corner "bites". bar radius=card radius=12. popout.c
dc_popout_bar_adjacent (10 callers) = margin bar_height+8gap + pivot at bar edge. nvgRoundedRectVarying
exists (per-corner radii). Config: NEW `connected_frame`/`connectedFrame` default false (NOT reuse
frame_enabled). Settings toggle beside "Rounded screen corners" (~L1266).

## MUST-have: G1 gap removal (1px seam overlap replaces 8px gap), G2 per-corner radii (near=0 square,
far=12), G3 connectors (2 concave quarter-circle fillets radius 12 flaring from near edge along bar
underside — sells "emerges from bar"; technique = frame.c draw_corner hole-punch), G4 chrome continuity
(shadow scissored to not darken bar; 3-side outline ending at connector tips). Convert control-center +
dashboard first, then rest mechanically.

## Approach (top bar; bottom mirrors): popout.c connected-aware: margin = bar_height - SEAM(1) - pad_near
(near pad=0 in connected mode), side-margin clamp >= bar_spacing+ccr, new dc_popout_chrome_pads(cfg,
*near,*side,*far) so panels stop hardcoding pad=6 (side pad=12 for connectors → surface widens 2*(12-6)).
Card = nvgRoundedRectVarying(0,0,12,12) top / swap bottom. New dc_material_bg_fill_card_varying(). New
src/ui/connected.c/.h: dc_connected_card_chrome(vg,render,w,h,pads,bottom_bar) = fill+connectors(hole-
punch quarter circles)+scissored-shadow+open-3-side-outline, with legacy floating branch when off →
single call site both modes. Animation unchanged (pivot=near edge keeps seam glued).

## Tasks (Sonnet each): T1 config key+settings toggle (config.c/h+settings.c) — no behavior. T2 chrome
foundation (material_bg varying + connected.c/h + popout.c/h pads/seam) — MUST be no-op when off. T3
control-center convert (flagship). T4 dashboard convert (‖T3). T5 batch: notifcenter/processes/
clip_picker/tray_menu/battery_popout. T6 batch: launcher/notepad/settings. T7 dock (nice-to-have). T8
fullscreen-hide (nice-to-have, needs niri is_fullscreen — verify field exists, degrade if absent). T9
polish (frame corners surface_container, connector lerp).
Serialize: config.c/settings.c(T1), popout.c/material_bg/connected.c(T2), then per-panel parallel.

## Risks: 10 call sites hardcode pad=6 in LAYOUT math (keep content rect identical, only surface size+
near/side pads change); seam color mismatch at bar_transparency<1 (draw 1px near strip in bar fill);
fractional-scale hairlines (use logical 1px overlap-not-abut); T2 must be provably no-op when off.
