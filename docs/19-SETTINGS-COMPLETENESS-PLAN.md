# 19 — Settings Completeness Plan (GNOME parity audit + Displays/Night-Light/Firewall/Printers deep design)

Status: research/plan only, no code changed. Continues the `14-COMPLETION-PLAN.md` W1–W5 numbering
with a proposed **W6** wave. Template follows `18-WIFI-BT-PLAN.md` (have vs missing → mechanism map →
sized wave plan).

## 0. TL;DR

DankC's Settings (`src/ui/settings.c`, 26 tabs) already covers the *shape* of GNOME Settings but several
tabs are UI stubs around little or no backend, and one whole class of GNOME functionality — mutating
system state that needs root (firewall, printer admin) — has no privilege-elevation story yet at all.
Ranked by daily-use impact, the gaps are:

1. **Displays** — shallow (`tab_displays`, settings.c:1874-1902): brightness slider + one hardcoded
   4000K night toggle. No monitor list, no resolution/refresh/scale, no arrangement, no rotation, no
   VRR, no enable/disable per output. niri IPC (`src/niri/niri.c`) is wired for workspaces/windows only
   — the `Outputs` request/`Output` action are documented in `docs/03-SERVICES.md §12` but **zero code
   calls them**. This is the single biggest gap for a multi-monitor daily user.
2. **Night Light** — currently a duplicated one-shot `pgrep/pkill/gammastep -O 4000 &` in two places
   (`settings.c:845-863` and `src/main.c:466-468`). No temperature slider, no schedule, no
   sunset-to-sunrise automation. GNOME users touch this weekly (evening toggle) — high daily value, low
   effort to fix properly.
3. **Firewall** — does not exist as a panel at all. Genuinely useful for a "daily-use completeness" bar
   only in the loose sense that many users check it exists/is on; effort is moderate, value is
   real-but-thinner than Displays/Night-Light. Needs a **new privilege-elevation mechanism** (see §8) —
   this is the first mutating settings feature dankc has ever needed root for.
4. **Printers** — read-only list only (`tab_printer`, settings.c:3034-3047, parses `lpstat -p`). Adding
   default-printer-select + test-page covers the daily 80% cheaply and needs **no root** at all
   (`lpoptions -d` is a per-user operation).
5. **Mouse/Touchpad/Keyboard** — does not exist as a panel. niri's `input {}` KDL block covers everything
   GNOME's Mouse & Touchpad / Keyboard panels expose (pointer speed/accel, natural-scroll, tap-to-click,
   dwt, repeat-rate/delay, xkb layout). This is pure file I/O (same shape as the existing Window Rules
   KDL parser/writer, settings.c:2039-2741) — no new IPC, no root. Should probably rank above Firewall.
6. Everything else GNOME has (Wi-Fi join UI, Online Accounts, Sharing, Accessibility, Color/ICC, full
   Region&Language/i18n, multi-user account management) is explicitly out of scope by dankc's own design
   decisions already recorded in `docs/07-GAPS-AND-DECISIONS.md` and `docs/08-SETTINGS-UI.md` (single-user
   desktop, no full i18n stack, defers to system tools for calibration/account management) — confirmed
   correct calls, not re-litigated here except where noted.

Proposed order of work (daily-use impact ÷ effort, root-needing work batched together since it shares
the new privilege-elevation plumbing): **Night Light → Displays → Mouse/Touchpad/Keyboard → Printers →
Firewall → Date&Time NTP/timezone → Power idle/lid**. Full sizing in §9.

---

## 1. Method

- Local: read `src/ui/settings.c` (3816 lines, all 26 tabs), `src/services/*` (power.c dual-backend
  pattern, polkit.c agent, niri IPC in `src/niri/niri.c`+`niri.h`), `docs/03-SERVICES.md`,
  `docs/07-GAPS-AND-DECISIONS.md`, `docs/08-SETTINGS-UI.md`, `docs/09-DMS-SETTINGS-INVENTORY.md`,
  `docs/14-COMPLETION-PLAN.md`, `docs/18-WIFI-BT-PLAN.md`, `AGENTS.md`, `CONVENTIONS.md`, `meson.build`,
  `Makefile`.
- Web: current `gnome-control-center` panel set (GNOME 47/48, GitLab source tree + Arch man page), niri
  wiki (IPC, Configuration:-Outputs, Configuration:-Input), gammastep/wlsunset man pages, ufw/firewalld
  docs, CUPS `lpstat`/`lpoptions`/`lpadmin`/libcups docs. Citations inline per section.

---

## 2. GNOME Control Center panel audit

GNOME's control center is organized as `applications, background, bluetooth, color, display, keyboard,
mouse, multitasking, network, notifications, online-accounts, power, printers, privacy, search, sharing,
sound, system, universal-access, wacom, wellbeing, wifi, wwan`, with `system` and `privacy` now hub
panels (`system` → about/datetime/region/remote-desktop/secure-shell/users; `privacy` →
camera/diagnostics/firmware-security/location/screen/usage/bolt). Source:
[gnome-control-center panels tree](https://gitlab.gnome.org/GNOME/gnome-control-center/-/tree/gnome-48/panels),
[Arch man page](https://man.archlinux.org/man/gnome-control-center.1.en).

| GNOME panel | Daily/weekly settings a normal user touches | dankc has it? | Gap |
|---|---|---|---|
| Wi-Fi | on/off, join network+password, forget, hidden network | `tab_network` (shallow, radio toggle + read-only status) | **By design** — defers scan/join to Control Center per settings.c comment. Revisit only if user wants it in-shell (`18-WIFI-BT-PLAN.md` already scoped this). |
| Network (wired/VPN) | wired toggle, VPN connect, proxy (rare) | Not present beyond Wi-Fi radio | Low priority; VPN daily-use is minority use case. |
| Bluetooth | on/off, pair, forget, discoverable | `tab_bluetooth` (medium: power toggle, paired list, connect/disconnect) | Adequate for daily use. |
| Background/Appearance | wallpaper, light/dark, accent color | `tab_personalization` + `tab_theme_colors` (deep + shallow-stub respectively) | Light theme rendering itself not implemented (`tab_theme_colors` is an explicit stub) — pre-existing known gap, not re-scoped here. |
| Notifications | DND, per-app toggle, banners | `tab_notifications` (medium: DND, timeouts, sounds) | Missing **per-app** notification toggle — moderate daily value, own future doc. |
| Search | which apps show in search, provider order | Not present (no "Activities search" concept in dankc's launcher) | N/A — different launcher paradigm, not a real gap. |
| Multitasking | hot corner, workspace behavior, all-displays workspaces | Not present | niri handles workspaces itself via its own config; low value to duplicate in dankc UI. Niche. |
| Applications (default apps + Flatpak permissions) | default browser/email/etc, per-app sandbox permissions | `tab_default_apps` (**deep**, 19-category MIME manager) | Flatpak permission toggles missing — niche (portal-level), skip. |
| Privacy & Security (screen lock, location, camera, mic, usage) | auto-lock timeout, location services, camera/mic access | `tab_lockscreen` covers auto-lock subset only | Camera/mic/location toggles are portal/xdg-desktop-portal territory, not simple CLI — **defer**, high effort low certainty. |
| Online Accounts | add account, per-service sync toggle | Not present | Correctly out of scope — needs GOA-equivalent backend, not a lightweight-shell fit. |
| Sharing | file sharing, remote desktop, media sharing | Not present | Niche for target user (power users on niri rarely want GNOME-style sharing services). Low priority. |
| Sound | output/input device+volume, per-app volume, alert sound | `tab_audio` (medium, wpctl-backed) | Per-app volume mixer missing — moderate value, own future doc, not deep-dived here (out of the 4 requested topics). |
| Power | profile (Balanced/Performance/Power Saver), screen blank, suspend/lid | `tab_power` (**shallow UI over a genuinely deep dual-backend service**, power.c) | Missing idle/blank timeout, suspend timing, lid-close behavior — see §7.6. |
| **Displays** | resolution, refresh rate, scale, orientation, arrangement, primary, **Night Light** | `tab_displays` (**shallow**: brightness + 1 night toggle) | **Deep-dived below, §3.** |
| Mouse & Touchpad | pointer speed, natural scroll, tap-to-click, dwt, handedness | **Not present at all** | **Deep-dived below, §7.1** (not one of the 4 headline topics but cheap+high-value; included). |
| Keyboard | layout, repeat rate/delay, shortcuts | Not present (layout/repeat only; shortcut rebinding out of scope) | See §7.1. |
| **Printers** | add/remove, default, job queue, test page | `tab_printer` (**shallow, read-only** `lpstat -p` list) | **Deep-dived below, §6.** |
| Color/ICC | calibrate, assign profile per display | Not present | Correctly niche — power-user feature, rarely opened weekly even by GNOME users. Skip. |
| Region & Language | locale, formats, input sources | `tab_locale` (shallow: first-day-of-week + read-only `$LANG`/`$TZ`) | Real i18n (locale switching, format packs) explicitly out of scope per `07-GAPS-AND-DECISIONS.md`. Input-source *adding* folds into Keyboard panel design (§7.1) since niri's `xkb{}` block is the same mechanism. |
| Accessibility | high contrast, large text, screen reader, cursor size | Not present | Real daily-use for a meaningful minority, not "most users" — niche relative to the other gaps but worth a cheap add: large-text (reuses `tab_typography`'s font-scale, already exists!) + cursor-size (niri `cursor{}` block) could be a tiny addition later. Not sized in this plan. |
| Users | add/remove/switch account | `tab_users` (shallow, read-only, **by design** — single-user desktop decision already recorded) | Correct as-is. |
| Date & Time | NTP toggle, timezone, manual set, 12h/24h | `tab_time` (shallow: 3 format toggles only, **no NTP/timezone control**) | **Deep-dived below, §7.2** — cheap, `timedatectl`-based. |
| About | hostname, OS version, hardware, disk usage | `tab_system` (shallow: hostname/kernel/uptime + autostart toggle) + `tab_about` (static) | Hostname is read-only in dankc; GNOME lets user rename device — minor, cheap add if desired. |
| **Firewall** (not a GNOME panel — Ubuntu ships `gufw` separately; included per explicit ask) | ufw/firewalld status + on/off | Not present | **Deep-dived below, §5.** |

---

## 3. Deep design: Displays (niri IPC)

### Current state
`tab_displays` (settings.c:1874-1902) only does sysfs backlight (`/sys/class/backlight`, set via
logind `SetBrightness`) and the crude night-mode toggle. `src/niri/niri.c` (423 lines) has a live
`EventStream` socket connection for workspaces/windows (`dc_niri_connect()`, niri.c ~284) using **cJSON**
(`#include "cJSON.h"`, already a project dependency — no new library needed), plus fire-and-forget
one-shot actions like `dc_niri_focus_workspace()` (niri.c:366) which **fork+execlp("niri","msg","action",
...)** — i.e. dankc already shells out to the `niri` CLI binary for mutating one-shot actions rather than
opening a second raw socket connection. `docs/03-SERVICES.md §12` documents the wire protocol
(`NIRI_SOCKET` env var, newline-delimited JSON, `{"Ok":...}`/`{"Err":...}`, unit-variant requests as bare
strings e.g. `"Outputs"`, struct-variant as single-key objects e.g.
`{"Output":{"output":"DP-1","action":{"On":{}}}}`) but no code calls `Outputs` today.

### Mechanism

**Read (enumerate monitors):** `niri msg --json outputs` (or a raw one-shot socket request
`"Outputs"\n` on `$NIRI_SOCKET`, parsed the same cJSON way `dc_niri_connect()`'s EventStream parser
already does — reuse the `json_num`/`json_bool`/`json_str` helpers at niri.c:47-70). Returns a map keyed
by connector name, each value:
```json
{
  "eDP-1": {
    "name": "eDP-1", "make": "...", "model": "...", "serial": "...",
    "modes": [{"width":1920,"height":1080,"refresh_rate":60030,"is_preferred":true}, ...],
    "current_mode": 0,
    "vrr_supported": false, "vrr_enabled": false,
    "logical": {"x":0,"y":0,"width":1920,"height":1080,"scale":1.0,"transform":"Normal"}
  }
}
```
(Field names per niri's `niri-ipc` crate / wiki; treat unknown fields/variants as forward-compatible
noise per the project's own versioning note in `03-SERVICES.md §12` — same rule GNOME/niri itself states:
only the **JSON** mode is a stable contract, human-readable `niri msg outputs` text is not.)
Source: [niri IPC wiki](https://github.com/YaLTeR/niri/wiki/IPC),
[niri_ipc::OutputAction docs.rs](https://docs.rs/niri-ipc/latest/niri_ipc/enum.OutputAction.html).

**Write, runtime (applies immediately, session-only unless also persisted):**
`niri msg output <name> <action>` — fork+execlp shape identical to `dc_niri_focus_workspace()`:
- `mode <WxH@refresh>` or `mode auto` — resolution/refresh-rate picker
- `scale <float>` — e.g. `1.0`, `1.25`, `1.5`, `2.0` (fractional supported)
- `transform <normal|90|180|270|flipped|flipped-90|flipped-180|flipped-270>` — orientation/rotation
- `position x=<v> y=<v>` (or `position off` to let niri auto-arrange) — monitor arrangement
- `on` / `off` — enable/disable
- `vrr` (on/off toggle, if `vrr_supported`) — variable refresh rate

**Write, persistent:** append/rewrite an `output "<name>" { ... }` block in
`~/.config/niri/config.kdl` — same KDL-editing shape dankc already has for Window Rules
(`tab_window_rules`, settings.c:2039-2741, tolerant block scanner + explicit backed-up `include` line).
Reuse that parser's approach rather than writing a second one from scratch:
```kdl
output "eDP-1" {
    mode "1920x1080@60.030"
    scale 1.5
    transform "normal"
    position x=0 y=0
    variable-refresh-rate on-demand=true
}
```
Source: [niri Configuration: Outputs wiki](https://github.com/YaLTeR/niri/wiki/Configuration:-Outputs).

### Panel design

- New `services/display.c`/`display.h`: `dc_display_list()` → array of `dc_display_output {name, make,
  model, modes[], current_mode_idx, scale, transform, x, y, enabled, vrr_supported, vrr_enabled,
  is_primary}`. One-shot socket-or-CLI read (`niri msg --json outputs`, popen'd and cJSON-parsed —
  matches the existing `xdg_query()` popen-first-line-reader convention at settings.c:703, just reading
  the whole stdout instead of one line).
- Rewrite `tab_displays`: monitor list (tabs/cards, one per connector) → per-monitor: resolution
  dropdown (from `modes[]`, mark preferred), refresh-rate paired with resolution, scale stepper
  (0.25 increments, common presets 1.0/1.25/1.5/2.0), orientation segmented (4 rotations + 2 flipped,
  collapse to just the 4 primary if flipped is rarely used), enable/disable toggle, VRR toggle (only
  shown if `vrr_supported`), **arrangement**: a simple 2-axis relative-position control ("left of /
  right of / above / below \<other monitor\>" segmented control rather than a full drag-to-position
  canvas — cheaper to build correctly than a mini-canvas widget, and covers the real daily need since
  dankc doesn't have a canvas/drag-widget primitive per the shared widget kit at settings.c:294-649).
  Primary-display selection = which output gets `position x=0 y=0`.
- Runtime actions apply immediately via fork+execlp `niri msg output ...` (instant visual feedback);
  a "Save as default" action (or auto-save on every change, matching the rest of settings.c's
  `c->changed`-flag-triggers-save-on-click convention) writes/updates the matching `output {}` KDL block
  so it persists across niri restarts — mirroring Window Rules' write-back pattern exactly.
- Mirror mode: out of scope for v1 (niri doesn't have a first-class "mirror" toggle the way `xrandr
  --same-as` does; achieving it means setting matching mode+position+scale on two outputs manually,
  which the arrangement UI already allows without special-casing).

---

## 4. Deep design: Night Light

### Current state
Two duplicated one-shot toggles (`settings.c:845-863`, `main.c:466-468`): `pgrep -x gammastep` to check
state, `pkill -x gammastep` to turn off, `gammastep -O 4000 &` to turn on — fixed 4000K, no gradual
warmth, no schedule, no slider.

### Mechanism choice: **wlsunset over gammastep**
Both use `wlr-gamma-control-unstable-v1` and work on niri. But for a C daemon that needs a live
**toggle** (warm/cool/auto) rather than kill-and-relaunch-with-new-flags:
- **wlsunset** responds to **`SIGUSR1`** at runtime, cycling forced-day → forced-night → automatic —
  i.e. dankc gets a free toggle primitive (`kill(pid, SIGUSR1)`) without restarting the child process.
  Flags: `-T <day_temp>` `-t <night_temp>` (defaults 6500/4000), `-l <lat> -L <lon>` for automatic
  sunset/sunrise, or `-S <HH:MM> -s <HH:MM>` for manual sunrise/sunset times (mutually exclusive with
  `-l/-L`), `-d <seconds>` transition duration (manual mode only), `-g <gamma>`.
  Source: [wlsunset man page](https://www.mankier.com/1/wlsunset),
  [wlsunset source/sourcehut](https://sr.ht/~kennylevinsen/wlsunset/).
- **gammastep** has no documented live-toggle signal or control socket — its manual-override path is
  kill-and-relaunch with new `-O`/`-t` flags. Its edge is `-l geoclue2` for auto-location without the
  user typing lat/lon, and finer `-b`/`-g` brightness/gamma control.
  Source: [gammastep man page](https://man.archlinux.org/man/gammastep.1).

**Recommendation:** use **wlsunset** as the long-running child for the "Schedule: sunset→sunrise"
mode (needs `-l`/`-L`, user enters lat/lon same textfields `tab_weather` already has at settings.c:1362 —
reuse those values instead of asking twice), and for "Schedule: manual times" mode (`-S`/`-s`). For the
temperature **slider** itself (the headline ask — a continuous 6500K↔2700K control, not just on/off),
launch wlsunset with `-T <slider_value> -t <slider_value>` (same value both ends) whenever the user is in
"manual/always-on" mode, i.e. treat the slider as directly setting the forced temperature by relaunching
wlsunset with new `-T/-t` args on slider release (debounced, not per-frame) — SIGUSR1 toggling is for the
day/night/auto tri-state once a schedule exists, not for continuous slider drags.

### Panel design
New `services/nightlight.c`/`nightlight.h`: `dc_nightlight_enabled()`, `dc_nightlight_set(bool on)`,
`dc_nightlight_set_temp(int kelvin)`, `dc_nightlight_set_schedule(mode, ...)`. Track the spawned pid the
same way the existing `pgrep -x`-based check does today (or store the pid dankc itself forked, avoiding
the pgrep round-trip entirely — preferred, since dankc now owns the process it spawns).
`tab_displays` gets a "Night Light" section: on/off toggle, temperature slider (2700K–6500K, label shows
Kelvin), schedule segmented (Manual / Sunset to Sunrise), time pickers (manual mode) or read-only
computed sunset/sunrise display (schedule mode, needs lat/lon — reuse `tab_weather`'s).

---

## 5. Deep design: Firewall (ufw + firewalld dual backend)

### Pattern to replicate: `services/power.c`
`power.c`/`power.h` is the exact template: `dc_power_backend` enum (NONE/PPD/TUNED_DBUS/TUNED_CLI),
lazy `probe_backend()` (power.c:89-103) that checks D-Bus name ownership first
(`bus_name_has_owner(g_system, "org.freedesktop.UPower.PowerProfiles")`, power.c:56-73) then falls back
to a CLI presence check (`system("command -v tuned-adm ...")`), caches the result in a static global, and
folds every backend onto **one** UI-facing enum/struct so `tab_power` never branches on backend.

For firewall: `dc_firewall_backend` enum `{NONE, UFW, FIREWALLD}`. Detection order:
1. `systemctl is-active firewalld` (pure systemd query, **no root needed**, cleanest non-root status
   check) → if active, backend = FIREWALLD.
2. else `command -v ufw` exists → backend = UFW (note: `ufw status` itself needs root; the one
   **non-root-readable** signal is `/etc/ufw/ufw.conf`'s world-readable `ENABLED=yes|no` line — parse
   that directly for an unprivileged initial status read instead of shelling to `ufw status`).
3. else NONE → hide the tab entirely (matches `tab_printer`'s existing pattern of just showing an empty
   state when no backend is present).

Sources: [firewalld firewall-cmd man](https://firewalld.org/documentation/man-pages/firewall-cmd.html),
[UFW community docs](https://help.ubuntu.com/community/UFW),
[/etc/ufw/ufw.conf non-root read workaround](https://github.com/openclaw/openclaw/issues/30361).

### Operations (all mutating ones need root — see §8 for the new elevation mechanism)
| Action | ufw | firewalld |
|---|---|---|
| Status (non-root) | parse `/etc/ufw/ufw.conf` `ENABLED=` | `systemctl is-active firewalld` |
| Status (detailed, root) | `ufw status verbose` | `firewall-cmd --state` + `--get-active-zones` |
| Enable/disable | `pkexec ufw enable` / `disable` | `pkexec systemctl enable --now firewalld` / `disable --now` |
| List allowed services | `ufw status` (parse `ALLOW` lines; no JSON mode exists) | `firewall-cmd --zone=<active> --list-services` |
| Allow common service | `pkexec ufw allow ssh` | `pkexec firewall-cmd --zone=<active> --add-service=ssh --permanent` then `pkexec firewall-cmd --reload` |
| Deny | `pkexec ufw deny <svc>` | `--remove-service=` equivalent |

### Panel design
New tab (`tab_firewall`) or fold into `tab_network` as a sub-section — recommend **new tab** for
discoverability, matching how GNOME treats it as conceptually separate even though Ubuntu doesn't ship it
in `gnome-control-center` proper (gufw is a separate app; dankc integrating it is a value-add over stock
GNOME, not just parity). Content: status row (On/Off/Not installed), enable/disable toggle
(`pkexec`-gated), a fixed shortlist of common services (SSH, HTTP, HTTPS, Samba, mDNS) as toggles calling
the allow/deny commands above — full custom port/rule editing is explicitly punted (matches the
"realistic scope" instruction — daily use is "is it on" + "let SSH through", not a rule editor).

---

## 6. Deep design: Printers (CUPS)

### Current state
`tab_printer` (settings.c:3034-3047) parses `lpstat -p` into name+status rows, read-only, with a comment
punting to system-config-printer/CUPS web UI for anything else.

### Mechanism
All via shelling out (no libcups linkage — matches the project's existing "shell out to CLI, don't link
heavy client libs" pattern used everywhere else in `services/`):
- `lpstat -p` — printer list + enabled/disabled status (already used).
- `lpstat -d` — current default.
- **`lpoptions -d <name>`** — set default. Confirmed **per-user, no root needed** (writes
  `~/.cups/lpoptions` when run as the invoking user) — this is the single most valuable addition and it's
  free of the privilege-elevation problem entirely.
  Source: [CUPS lpoptions](https://manpages.ubuntu.com/manpages/trusty/man1/lpoptions.1.html).
- `lp -d <name> /usr/share/cups/data/testprint` — print test page (no root; CUPS ships this file on any
  system with `cups` installed).
- `lpstat -o` — job queue (per-destination or all); no cancel/pause wired in v1 (stretch: `cancel <job-id>`
  is also non-root for the job's own owner).
- Ink/toner levels: **confirmed not exposed by CUPS at all** — vendor/driver-specific (HPLIP etc.), no
  generic API. Correctly out of scope.
- Add/remove printer queues (`lpadmin`) needs root or `lpadmin` group membership — **punt to system tool**
  per the instruction's own guidance (list+default+test-page is the daily 80%).
  Source: [CUPS lpstat](https://www.cups.org/doc/man-lpstat.html),
  [CUPS Programming Manual](https://www.cups.org/doc/cupspm.html) (confirms no libcups ink API either).

### Panel design
Extend `tab_printer`: per-printer row gets a "Set as default" action (star icon, calls `lpoptions -d`) and
a "Print test page" action (calls `lp -d`). Add a small job-queue section below the printer list
(`lpstat -o`, read-only, auto-refreshes same cadence as the existing list). No root needed for any of
v1 — ships independently of the Firewall/polkit work in §8.

---

## 7. Other useful additions

### 7.1 Mouse & Touchpad + Keyboard (niri `input {}` — **recommend prioritizing above Firewall**)
niri's `input {}` KDL block (`~/.config/niri/config.kdl`) covers essentially 1:1 what GNOME's two panels
expose: `touchpad { tap; dwt; natural-scroll; accel-speed <float>; accel-profile "flat"|"adaptive";
click-method "clickfinger"; }`, `mouse { natural-scroll; accel-speed; accel-profile; left-handed; }`,
`keyboard { xkb { layout; variant; options; } repeat-delay <ms>; repeat-rate <cps>; }`.
Source: [niri Configuration: Input wiki](https://github.com/YaLTeR/niri/wiki/Configuration:-Input).
This is pure KDL file I/O, **zero root**, same shape as Window Rules' parser (settings.c:2039-2741) — new
tab `tab_input`: pointer-speed slider (maps to `accel-speed`, -1.0..1.0), natural-scroll toggle
(mouse+touchpad separately), tap-to-click toggle, disable-while-typing toggle, left-handed toggle,
keyboard repeat rate/delay sliders, xkb layout/variant text or dropdown (if a layout list is available —
`localectl list-x11-keymap-layouts` gives a static list without root). **High value, no new risky
plumbing (no root, no new IPC) — should be sequenced ahead of Firewall.**

### 7.2 Date & Time (cheap, `timedatectl`)
`tab_time` today is 3 display-format toggles with no NTP/timezone control. `timedatectl status`
(read, no root) → parse `NTP service: active/inactive`, `Time zone: ...`. Mutating:
`pkexec timedatectl set-ntp true|false`, `pkexec timedatectl set-timezone <TZ>` — both **need root**
(same §8 mechanism as Firewall), so bundle into the same wave. Timezone picker can source its list from
`timedatectl list-timezones` (no root). Small, high daily-relevance (NTP toggle rarely touched but
timezone matters when traveling).

### 7.3 Power — idle/blank/lid (extends existing deep backend, `power.c`)
`tab_power` only exposes the 3-way profile segmented control today. GNOME's daily-relevant additions:
screen-blank timeout and suspend-on-idle are **logind/systemd-logind** territory (`logind.conf`
`IdleAction`/`IdleActionSec`, requires root to edit `/etc/systemd/logind.conf` — or read via
`loginctl show-session` at runtime without root for display purposes). Lid-close behavior
(`HandleLidSwitch=`) is the same file. Since dankc already has `services/logind.c` for other
logind calls, this is additive to an existing service rather than a new one. Root needed for persistent
changes → batch into the Firewall/Date-Time elevation wave.

### 7.4 Accessibility (niche but near-free)
Large text is **already possible** — `tab_typography`'s existing font-scale slider (settings.c:1277-1298)
IS the large-text control, just not labeled/discoverable as "Accessibility." Consider only a label/cross-
reference, not new code. Cursor size (niri `cursor { size <n>; }` in config.kdl) is a 10-minute addition if
ever prioritized. High-contrast/screen-reader are out of scope (no Orca-equivalent, no theme-invert
infra) — correctly niche, not sized in this plan.

### 7.5 Region & Language depth
Confirmed correctly out of scope beyond what `tab_locale` already does — real locale-switching requires
regenerating `/etc/locale.gen`/`locale-gen` (root, distro-specific, high blast-radius for a shell to touch)
per `07-GAPS-AND-DECISIONS.md`'s existing call. Not re-scoped.

### 7.6 Sharing / Color / Online Accounts / Accessibility (screen reader) / full multi-user
All confirmed **correctly out of scope** for a lightweight single-user niri shell — either require heavy
subsystems dankc deliberately doesn't depend on (GOA, Orca, ICC color management stack) or contradict the
project's single-user design decision already recorded in docs. Not sized in this plan.

---

## 8. New mechanism required: privilege elevation for mutating root actions

This is new territory — dankc has never needed to run a *mutating* command as root before. Two relevant
existing pieces, neither of which is directly reusable as-is:

- `services/polkit.c` (611 lines) is a polkit **authentication agent** — the thing that *answers*
  polkitd's prompts when some other program calls `pkexec`/hits a polkit check. It registers on the
  system bus as `org.freedesktop.PolicyKit1.AuthenticationAgent`, and on `BeginAuthentication` shows
  `ui/polkit_modal.c`'s password dialog and drives `polkit-agent-helper-1` via stdin/stdout. It is **not**
  a "run this command as root" helper dankc calls on its own behalf.
- No existing `pkexec` call site anywhere in `src/` today (`grep -rn pkexec src/` is empty) — every
  current mutating shell-out (nmcli, bluetoothctl, tuned-adm, xdg-mime, wpctl) runs unprivileged as the
  logged-in user.

**Key insight (reuse, not new UI needed):** if dankc's own polkit agent (`polkit.c`) is registered and
active for the session (it already is, whenever dankc is running as the shell), then a plain
`pkexec <command>` call from settings.c — via the exact same `run_detached()` helper already used
everywhere (settings.c:666, fork `/bin/sh -c ...`, honors `DANKC_SETTINGS_DRYRUN`) — will have its
polkit authorization prompt answered by **dankc's own already-built password modal**
(`ui/polkit_modal.c`), not a foreign GTK/KDE dialog. No new agent-side code is needed; only the
**call site** (`pkexec ufw enable`, `pkexec timedatectl set-ntp true`, etc.) is new.

**Security-hardening recommendation:** rather than `pkexec`-ing raw shell strings with user-influenced
substrings (e.g. a service name into `ufw allow <svc>`), ship a small fixed-verb helper script
`/usr/lib/dankc/dankc-privhelper` with a matching
`/usr/share/polkit-1/actions/org.dankc.settings.policy` action definition, and always invoke
`pkexec /usr/lib/dankc/dankc-privhelper <verb> [validated-arg]` where `<verb>` is one of a small
whitelist (`fw-enable`, `fw-disable`, `fw-allow-service`, `fw-deny-service`, `ntp-enable`, `ntp-disable`,
`set-timezone`, `set-lid-action`, ...). This gives a nicer polkit prompt (custom action description
instead of the generic "Run a program as another user") and avoids shell-injection risk from any
textfield-sourced argument reaching `pkexec` directly. Sized as its own small (S) task in §9 since it's
shared infrastructure for Firewall + Date&Time NTP + Power idle/lid, all of which need root.

Printers (§6) and Displays (§3, runtime `niri msg output` actions run as the logged-in user, no root)
explicitly do **not** need this mechanism — call it out clearly in the wave plan so they aren't
accidentally gated behind it.

---

## 9. Wave plan (proposed **W6**, continuing `14-COMPLETION-PLAN.md`)

Conventions to follow for every item (per `AGENTS.md`/`CONVENTIONS.md`, already audited):
- New tab = add enum value to `s_tab` (settings.c:101-129, alphabetical-ish with the rest) + a
  `{icon,"Label"}` row to `TABS[]` (settings.c:136-150, same order) + `static void tab_xxx(uictx*)` +
  a `case TAB_XXX: tab_xxx(c); break;` in `build_tab()`'s switch (settings.c:3127-3211). No separate
  registration table exists.
- New service = new `src/services/<name>.c`+`.h` (or `src/niri/` for the display extension), added to
  **`meson.build`**'s `src = files(...)` list (services block currently at meson.build:126-133) —
  the **Makefile already auto-globs** `src/services/*.c` (Makefile:82) so it needs no edit, but
  meson.build (the primary build per AGENTS.md) does.
- Every mutating action gates on a `DANKC_<AREA>_DRYRUN` env var (matches `DANKC_SETTINGS_DRYRUN`,
  `DANKC_POWER_DRYRUN`, etc.) for agent-driven/CI verification safety — non-negotiable given how
  DankC's own build-verification loop works (`DANKC_SETTINGS_TAB=<n>` / `_CLICK=x,y` scripted clicking,
  settings.c:3474-3527).
- Docs updated in the same commit as behavior per `AGENTS.md`'s Definition of Done.

| # | Item | New files | Size | Root/polkit? | Notes |
|---|---|---|---|---|---|
| W6.1 | Night Light rewrite | `services/nightlight.c/.h` | **M** | No | Replace both duplicated pgrep/pkill call sites (settings.c:845-863, main.c:466-468) with the new service; wlsunset spawn+SIGUSR1 toggle+slider-driven relaunch. |
| W6.2 | Displays: read-side (`niri msg --json outputs` → monitor list UI) | `services/display.c/.h` | **M** | No | Enumerate + show current mode/scale/transform/position per output; no writes yet. |
| W6.3 | Displays: write-side (mode/scale/transform/position/on-off/vrr, runtime + KDL persistence) | extends `services/display.c` + reuses Window-Rules'-style KDL writer | **L** | No | The bulk of the Displays effort; depends on W6.2. Arrangement UI = relative-position segmented control, not a drag canvas (no canvas widget primitive exists yet). |
| W6.4 | Mouse/Touchpad/Keyboard (`tab_input`) | `services/input_cfg.c/.h` (or fold into `services/display.c` since both edit `config.kdl`) | **M** | No | KDL `input{}` block read/write, same parser shape as Window Rules. Recommend doing **before** Firewall despite numbering — no new risky plumbing, high daily value. |
| W6.5 | Printers: default + test page + job queue | extends existing `tab_printer`, no new service file needed (just more `lpstat`/`lpoptions`/`lp` calls in settings.c, matching its existing inline-shell-out style) | **S** | No | Cheapest item in the whole plan; ship independently, first. |
| W6.6 | Privilege-elevation helper (`dankc-privhelper` + polkit `.policy`) | `packaging/dankc-privhelper` (script or tiny C binary) + `packaging/org.dankc.settings.policy` | **S** | N/A (this IS the mechanism) | Shared infra consumed by W6.7 and W6.8. Verify dankc's own `polkit.c` agent actually answers the prompt end-to-end (`DANKC_POLKIT_DEMO` path already exists in main.c for offline modal testing — extend it to cover a real `pkexec` round-trip once this lands). |
| W6.7 | Firewall panel (ufw + firewalld dual backend) | `services/firewall.c/.h`, new `tab_firewall` | **M** | **Yes** (via W6.6) | Detection pattern transplanted from `power.c`'s `probe_backend()`. Ship status+enable/disable+shortlist-services only (no custom rule editor). |
| W6.8 | Date & Time: NTP toggle + timezone picker | extends `services/logind.c` or new `services/datetime.c/.h`; extends existing `tab_time` | **S** | **Yes** (via W6.6) | `timedatectl` read is free; writes need W6.6. |
| W6.9 | Power: idle/blank timeout + lid-close action | extends `services/logind.c` + `tab_power` | **M** | **Yes** (via W6.6) | `/etc/systemd/logind.conf` edit + `systemctl restart systemd-logind` (or `loginctl`-only runtime knobs where available to avoid the restart). |

**Recommended sequencing:** W6.5 (Printers, trivial) → W6.1 (Night Light) → W6.2→W6.3 (Displays) → W6.4
(Input) → W6.6 (privhelper infra) → W6.7 (Firewall) → W6.8 (Date&Time) → W6.9 (Power idle/lid). This
front-loads every no-root item (fastest wins, matches the user's named priorities Displays/Night-Light
first anyway) and does the shared privilege-elevation plumbing exactly once before the three items that
need it, rather than three times ad hoc.

---

## 10. Open questions / risks

- **niri `Outputs` JSON schema drift**: not semver'd (per `03-SERVICES.md §12`'s own warning) — W6.2
  should verify field names against the actual running niri version via `niri msg --json outputs` before
  hardcoding cJSON key lookups, and tolerate unknown fields per the project's existing convention.
- **wlsunset vs gammastep**: recommendation above is wlsunset for its SIGUSR1 toggle; if geoclue2-style
  automatic-location-without-typing-coordinates turns out to matter more than the toggle ergonomics,
  gammastep is the fallback — both are commonly packaged, worth a quick `pacman -Ss`/`apt-cache search`
  check on target distros before locking the choice in the W6.1 implementation.
- **ufw has no JSON output mode** at all — parsing `ufw status`/`ufw show added` text is inherently
  fragile across ufw versions; firewalld's D-Bus/CLI is more scriptable. Worth flagging to the user that
  ufw support may be "status + on/off + a fixed allow/deny shortlist" only, not anything more structured,
  by design/necessity rather than laziness.
- **`dankc-privhelper`'s polkit `.policy` action** needs to be installed system-wide
  (`/usr/share/polkit-1/actions/`) by dankc's packaging (`packaging/` dir already exists) — this is a
  packaging-time install step, not something dankc can self-install at runtime; note in W6.6's task
  description so it isn't missed during the actual package build.
