# dankc Keybind Editing — Implementation Plan (2026-07-04)

Group-1 feature. Today keybinds_modal.c is a READ-ONLY cheat sheet (own tolerant KDL parser
kb_parse_config→kb_parse_file→kb_scan_binds_blocks→kb_parse_one_bind, follows includes). Make binds
EDITABLE (capture chord, add/remove/reset) writing a niri fragment — like niri_input.c/display.c.

## UX: new Settings tab TAB_KEYBINDS (settings.c), modeled on tab_window_rules() (wr_* ~L2860-3560,
the managed-vs-readonly CRUD template). Cheat-sheet overlay stays read-only, refactored onto the shared
service + gains a "managed" marker + footer hint pointing to Settings.
Capture flow: "Record shortcut" → s->kb_capture=true + create zwp_keyboard_shortcuts_inhibitor on the
settings surface (niri runs its own binds before layer clients, so ON_DEMAND interactivity isn't enough)
→ handle_key consumes all: Esc cancel; modifier-only keysyms ignored (live "Mod+Ctrl+…" display); other
key → read mods (super/ctrl/alt/shift) + LEVEL-0 base keysym of keycode (so Shift+2 records "2") → chord
(Super spelled "Mod") → pick action (3-way: dankc ctl preset / niri-verb preset / custom command) +
optional description (hotkey-overlay-title) → conflict line gates Add → persist.

## Service: src/services/keybinds.c/.h (NEW) — mirror niri_input.h conventions
dc_keybind { chord[64], action[192] (raw body verbatim, round-trips managed binds byte-stable),
title[96], props[96] (repeat/allow-when-locked/cooldown preserved), source[128], bool managed }.
- dc_keybinds_load(out,max,config_dir_override) — generalize kb_parse_file with per-file origin + raw
  body; managed = source is dankc-binds.kdl.
- dc_keybinds_persist(managed,n,override) — rewrite ~/.config/niri/dankc-binds.kdl (binds{ <chord>
  [hotkey-overlay-title=".."] [props] { <action>; } } + "Managed by DankC" header), ensure_include
  ("dankc-binds.kdl") w/ timestamped .bak (verbatim from niri_input.c), niri validate (SIGCHLD dance).
  NEW vs niri_input: ROLLBACK — snapshot prior fragment bytes, if validate FAILED restore + re-validate
  + report DC_KEYBINDS_VALIDATE_FAILED_ROLLED_BACK.
- dc_keybinds_last_validate(); dc_keybinds_normalize_chord (canonical mod order + xkb_keysym_from_name
  CASE_INSENSITIVE); dc_keybinds_find_conflict (over FULL list — dup chord across merged binds{} = hard
  niri error); dc_keybinds_chord_from_capture(base_keysym,super,ctrl,alt,shift,out) Super→"Mod";
  dc_keybinds_niri_actions() (move KB_NIRI_ACTIONS here), dc_keybinds_dankc_actions() (new ctl presets).
- Gate: DANKC_BINDS_DRYRUN=1. Add=append→persist; Remove(managed only)=drop→persist; Reset=empty
  fragment (never touches user binds). No config.json keys.

## wl.c/wl.h additions (T2):
- dc_wayland_super_down/alt_down (clones of ctrl_down w/ XKB_MOD_NAME_LOGO/ALT).
- record last_keycode in keyboard_handle_key; dc_wayland_base_keysym() via
  xkb_keymap_key_get_syms_by_level(keymap,keycode,layout,0,&syms) — level-0 keysym.
- protocol/keyboard-shortcuts-inhibit-unstable-v1.xml (Makefile wildcard auto-gens; ADD generated to
  meson.build), bind zwp_keyboard_shortcuts_inhibit_manager_v1 in registry (nullable like
  cursor_shape_manager), dc_wayland_shortcuts_inhibit(wl,surface)/uninhibit(wl). Degrade+hint if absent.
- key-repeat harmless (capture consumes first press + exits). No main.c dispatch changes.

## Task breakdown (one Sonnet each):
- **KB-T1 Service**: keybinds.c/.h (parser generalized from keybinds_modal + source/raw-body, normalize,
  conflict, persist+include+backup+validate+ROLLBACK, DANKC_BINDS_DRYRUN, config_dir_override, action
  tables) + tests/test_keybinds.c (Makefile target, parse fixtures/normalize/conflict/persist-roundtrip/
  rollback) + meson.build. — no deps.
- **KB-T2 Wayland capture**: wl.c/wl.h super/alt helpers + last-keycode + base_keysym + shortcuts-inhibit
  protocol XML + registry bind + inhibit API + meson.build. Parallel w/ T1 but SERIALIZE meson.build.
- **KB-T3 Settings tab** (settings.c ONLY): TAB_KEYBINDS cloned from tab_window_rules — STATUS/YOUR BINDS
  (managed removable)/EXISTING BINDS (readonly+source)/RESET/ADD (record-chord + capture state + live-mod
  + 3-way action selector from service tables + custom-cmd+description focus_fields + conflict line +
  Add/Replace); extend dc_settings_handle_key with capture-consumes-all; inhibitor create/destroy on
  capture start/stop + settings hide. Deps T1,T2.
- **KB-T4 Modal refactor** (keybinds_modal.c/.h): replace private parser with dc_keybinds_load; source
  verb tables from service; "dankc" managed marker + footer hint. Deps T1 (parallel w/ T3).
- **KB-T5 Verify+docs**: e2e DANKC_BINDS_DRYRUN capture/add/remove/reset; scratch service test; docs +
  print_keybinds() comment; confirm handle_key priority unchanged. Deps T3,T4.
Serialized: meson.build(T1→T2), settings.c(T3), wl.c/wl.h(T2), keybinds_modal.c(T4), main.c(T5 comments).

## Risks: niri validate failure→rollback (snapshot+restore); reserved binds need shortcuts-inhibitor
(degrade+hint if unsupported; warn on modifier-less chords for normal keys, allow bare Print/XF86/F-keys);
action vocab 3 classes w/ correct KDL quoting (escape "/\); Super→Mod + base-keysym-level0 + normalization;
live pickup via niri config-watch (+ optional `niri msg action load-config-file` after validate).
