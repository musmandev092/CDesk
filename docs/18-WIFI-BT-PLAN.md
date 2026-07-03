# DankC — Wi-Fi + Bluetooth Professional Management Plan

Research + recommendation + implementation plan for taking dankc's Wi-Fi and Bluetooth from "works"
(toggle, connect, basic pairing) to "GNOME Settings Network/Bluetooth-panel professional" — full
management, not just connect/toggle.

**Status**: design doc only. No code changed.

**Bottom line up front**: build it natively (**Option B**), extending `services/net.c`,
`services/bluez.c`, `ui/controlcenter.c`, and `ui/settings.c`. dankc already has the hard 60-70% done
(NetworkManager D-Bus event subscription, async scan, inline password entry, a registered BlueZ
pairing agent with passkey/confirm/authorize handling). Launching an external tool (Omarchy's model)
is the wrong move for dankc specifically, for reasons detailed in §3 — it fits Hyprland (no shell) but
actively fights a project whose entire premise is "be the shell." A **narrow hybrid escape hatch**
(one "Advanced..." button that shells out to `nm-connection-editor` for 802.1x/enterprise config
dankc won't reimplement) is worth keeping, but it is not the backbone of the design.

---

## 1. Omarchy / landscape research (verified)

### 1.1 What Omarchy actually uses, and why

Confirmed via the official Omarchy manual and the `basecamp/omarchy` GitHub discussions:

- **Wi-Fi**: [`impala`](https://github.com/pythops/impala), a Rust TUI built directly on **iwd**
  (`iwd`'s D-Bus API, not NetworkManager). Launched in a floating terminal (kitty) by clicking the
  Wi-Fi icon in Omarchy's `waybar`-style top bar.
- **Bluetooth**: [`bluetui`](https://github.com/pythops/bluetui), a TUI by the same author, built on
  BlueZ directly, same launch-in-terminal pattern.
- **Backend**: Omarchy defaults to **iwd** (not NetworkManager/wpa_supplicant). This is a known,
  discussed limitation: iwd's WPA2/WPA3-Enterprise (802.1x, e.g. eduroam) support is weaker, and
  several Omarchy discussion threads (#1454, #3359, #959) are users hitting exactly this wall and
  being told to switch their whole system to NetworkManager + wpa_supplicant + `nm-applet` +
  `nm-connection-editor` if they need enterprise auth — i.e. Omarchy's own community's answer to
  "I need professional-grade Wi-Fi" is **"stop using our default stack and adopt NetworkManager's own
  GUI tools instead,"** which is the opposite direction from "launch a nicer TUI."
- **Why this works for Omarchy at all**: Hyprland is a bare Wayland compositor with **no shell** — no
  status bar widgets with built-in menus, no control center, no settings app of its own. Omarchy's
  entire "desktop" is Hyprland + a collection of separately-launched programs (waybar, wofi/rofi,
  swaync, kitty, and these TUIs) glued together by keybinds. Spawning a terminal-hosted TUI is not a
  compromise there — it *is* the architecture. There is nothing "more integrated" to fall back to.

Sources: [The Omarchy Manual — TUIs](https://learn.omacom.io/2/the-omarchy-manual/59/tuis),
[bluetui as bluetooth manager · Discussion #3590](https://github.com/basecamp/omarchy/discussions/3590),
[iwd doesn't connect to my wifi · Discussion #3053](https://github.com/basecamp/omarchy/discussions/3053),
[Add 802.1x WiFi network support · Discussion #1454](https://github.com/basecamp/omarchy/discussions/1454),
[Better Wi-Fi Implementation (Hidden Network Support) · Discussion #3359](https://github.com/basecamp/omarchy/discussions/3359).

### 1.2 Contrast with dankc/DMS/GNOME

dankc, DMS, and GNOME Shell are not bare compositors — they **are** the shell: a persistent bar,
control center, notification center, settings app, lock screen, all one process (or one cohesive
QML/C tree) sharing a design language. GNOME Settings' Network/Bluetooth panels, and DMS's
`NetworkTab.qml`/`BluetoothTab.qml` + `NetworkPreferences`/quick-toggle popups, are native panels
in that same shell, not spawned external programs. dankc has already committed to this model (it has
its own control-center Wi-Fi/Bluetooth expandable sections, its own Settings tabs, its own BlueZ
pairing agent) — the question this doc answers is how far to take that, not whether to abandon it.

### 1.3 Tool landscape — weight, deps, backend, features

| Tool | Type | Backend | Deps pulled in | Visual fit in dankc | Feature completeness |
|---|---|---|---|---|---|
| **impala** | TUI (Rust, ratatui) | **iwd** only | Rust runtime (static bin, small), but needs iwd replacing NetworkManager | None — a terminal window, breaks Material look entirely | Scan/connect/known networks/hidden SSID; **no** WPA-Enterprise (iwd limitation), no per-network IP/DNS details UI |
| **bluetui** | TUI (Rust, ratatui) | BlueZ (D-Bus, same as dankc) | Rust runtime, small | None — terminal window | Scan/pair/connect/trust/remove; no battery/codec detail shown |
| **nmcli / nmtui** | CLI / ncurses TUI | NetworkManager (dankc's backend) | None (ships with NM) | nmtui: terminal window, ugly by comparison; nmcli: what dankc *already* shells out to | nmtui covers most NM features incl. 802.1x, but is a full-screen ncurses app, not embeddable |
| **iwgtk** | GUI (GTK3) | iwd | Full GTK3 + iwd | Separate window, non-Material theme, wrong backend | Decent iwd-native feature set, but same iwd/NM backend mismatch as impala |
| **blueman** | GUI (GTK3 + Python) | BlueZ | GTK3 + **Python3 + PyGObject + dbus-python**, its own applet/tray daemon | Separate window, heaviest of the lot | Most complete BT manager available (pairing, trust, audio profile switch, OBEX, network sharing) but drags in a Python stack for a single panel |
| **blueberry** | GUI (GTK3, C+meson, GNOME-ish) | BlueZ | GTK3 (no Python) | Separate window, GNOME (Adwaita) look | Solid subset of blueman's features, lighter than blueman but still a whole extra GTK app/process |
| **nm-applet / nm-connection-editor** | GUI (GTK3, official NM tools) | NetworkManager (matches dankc) | GTK3 (+ libnma) | Separate window; nm-applet is a tray icon+menu, not embeddable in a control center | The most complete NM-native editor available (802.1x, static IP, VPN, bonding) — this is genuinely the escape hatch for enterprise auth, not a replacement for the daily-driver panel |

**Key structural finding**: `impala`/`iwgtk` need **iwd**; dankc's `net.c` is built entirely on
**NetworkManager** (D-Bus `org.freedesktop.NetworkManager`, plus `nmcli` shell-outs as fallback).
Adopting impala would mean either running two competing network backends, or migrating dankc off
NetworkManager onto iwd system-wide — a much bigger and riskier change than writing more C, and one
that (per §1.1) loses WPA-Enterprise support in the process, which is a *regression* versus what
dankc already has (NM handles 802.1x natively; iwd's Enterprise support is limited/immature). The
BlueZ-based tools (bluetui, blueman, blueberry) don't have this backend conflict, but every one of
them is a **separate GTK or ncurses window** breaking the Material visual cohesion that is dankc's
whole reason for existing as a C reimplementation of DMS, and the GTK ones (blueman especially) pull
in a Python/GTK runtime for a feature dankc already has ~70% built natively in plain C + sd-bus.

---

## 2. dankc current-state gap analysis

Read in full: `src/services/net.c` (835 lines), `src/services/net.h`, `src/services/bluez.c` (802
lines), `src/services/bluez.h`, plus the Wi-Fi/Bluetooth sections of `src/ui/controlcenter.c` (2505
lines total) and `src/ui/settings.c` (3816 lines total, `tab_network`/`tab_bluetooth` around lines
1955-2017).

### 2.1 Wi-Fi — have vs missing

**Have** (`services/net.c`, `ui/controlcenter.c` network expand section, `ui/settings.c` `tab_network`):

- Event-driven SSID + live signal strength via NetworkManager D-Bus `PropertiesChanged` on the Wi-Fi
  `Device` + active `AccessPoint` objects (`dc_net_init`/`nm_resolve_ap_from_device`/
  `nm_read_ap_properties`) — zero-fork steady state, falls back to a cached `nmcli` popen if NM isn't
  reachable.
- Async scan list (`dc_net_wifi_scan`, fork+pipe+non-blocking-drain of
  `nmcli -t -f SSID,SIGNAL,SECURITY,IN-USE dev wifi list`), refreshed every 8s while the control-center
  network section is expanded — SSID, signal %, secured y/n, in-use, and a `known` flag
  cross-referenced against `nmcli connection show` names.
- Connect to open/known network in one click (`dc_net_wifi_connect` → `nmcli dev wifi connect <ssid>`,
  fire-and-forget, relies on NM reusing a saved connection's secrets).
- Inline password entry + connect for a secured, not-yet-known SSID (`dc_net_wifi_connect_psk`,
  async job with success/failure state machine parsing nmcli's stdout+stderr for an `Error:` line —
  there's no real exit-status channel since `SIGCHLD` is process-wide `SIG_IGN`).
- Wi-Fi radio on/off toggle (`nmcli radio wifi on|off`) and rfkill toggle, in both the control center
  tile and Settings' `tab_network`.
- `DANKC_WIFI_FAKE_AP` / `DANKC_WIFI_DRYRUN` debug hooks for screenshot-driven UI verification without
  touching a real network.

**Missing** (everything a GNOME-Settings-level Wi-Fi panel has that dankc doesn't):

1. **Known/saved connections list independent of a live scan** — dankc only shows saved networks that
   are *currently in scan range* (the `known` flag decorates a live AP row); there is no "Saved
   Networks" list you can browse/manage when out of range.
2. **Forget/delete a saved connection.**
3. **Auto-connect toggle** per saved network (NM's `connection.autoconnect` setting).
4. **Hidden SSID connect** (manual SSID entry — `nmcli dev wifi connect <ssid> password <p> hidden yes`
   equivalent, or the NM `AddAndActivateConnection` call with `802-11-wireless.hidden: true`).
5. **WPA3 is untested/unhandled explicitly** — dankc's connect path is security-agnostic (just SSID +
   PSK to nmcli), which happens to work for WPA2/WPA3-Personal, but there's no UI distinction and no
   path at all for **WPA/WPA2/WPA3-Enterprise (802.1x)** — that needs EAP method/identity/CA-cert/
   anonymous-identity fields nmcli's simple `dev wifi connect` doesn't expose; needs
   `AddConnection`/`Update` on `org.freedesktop.NetworkManager.Settings.Connection` with an
   `802-1x` settings group.
6. **Per-network detail view**: IP address, gateway, DNS servers, MAC (device's, and the option to
   randomize/spoof it), security type readout, connection uptime.
7. **Captive-portal hint** — NM exposes `Connectivity`/`ConnectivityCheckAvailable` on the manager and
   per-connection state; dankc surfaces neither.
8. **Airplane-mode-wide toggle** distinct from the per-radio Wi-Fi toggle already there (`nmcli
   networking off`/`NetworkManager.WirelessEnabled` vs `rfkill`).
9. **Secrets prompted by NetworkManager itself never reach dankc** — today, secrets only flow through
   dankc's own inline password field feeding straight into an `nmcli` invocation. There is **no
   registered NM Secret Agent**, so if some *other* path needs a secret (a saved connection whose
   password was cleared, a VPN sub-connection, 802.1x re-auth), NetworkManager has nowhere to ask and
   the connection just fails silently from dankc's perspective. This is the Wi-Fi analogue of the
   BlueZ pairing agent dankc already has for Bluetooth — see §4 Wave 4.
10. **Connection editor basics** (rename a saved connection, view/edit its stored settings) — dankc has
    no concept of a `Settings.Connection` object at all yet, only the live `Device`/`AccessPoint`
    objects.

### 2.2 Bluetooth — have vs missing

**Have** (`services/bluez.c`, `ui/controlcenter.c` bluetooth expand section, `ui/settings.c`
`tab_bluetooth`):

- Adapter power read (`Adapter1.Powered`) and toggle (`bluetoothctl power on|off`).
- Device enumeration via `GetManagedObjects` — paired-or-connected devices always, plus unpaired
  nearby devices while a discovery is active (`Discover` affordance, W3.1).
- Start/stop discovery (`Adapter1.StartDiscovery`/`StopDiscovery`), properly gated so unpaired devices
  drop out of the list once discovery stops.
- Connect/disconnect a paired device (`bluetoothctl connect|disconnect <mac>`, deliberately routed
  through `bluetoothctl` rather than a direct `Device1.Connect()` D-Bus call specifically *because*
  `bluetoothctl` already handles agent/pairing fallbacks dankc's own code doesn't reimplement for this
  path).
- **First-time pairing** (`dc_bluez_pair`): a real async `Device1.Pair()` → `Properties.Set(Trusted,
  true)` → `Device1.Connect()` chain, entirely in dankc's own C/sd-bus code (not shelled out).
- **A fully registered `org.bluez.Agent1` pairing agent** (`KeyboardDisplay` capability), handling
  `RequestConfirmation` (numeric-comparison "does NNNNNN match?"), `RequestAuthorization` (plain
  "pair with X?"), and `RequestPasskey` (type the code shown on the device) — with graceful
  degradation (`Just Works` devices unaffected) if another agent already owns the registration.
  `RequestPinCode` (legacy pre-SSP PIN pairing) is explicitly rejected — no UI for it yet.
- `DANKC_BT_FAKE_DEVICE` / `DANKC_BT_DRYRUN` debug hooks, including simulating an agent
  confirmation mid-pair, for screenshot-driven verification without real hardware.

**Missing** (everything a GNOME-Settings-level Bluetooth panel has that dankc doesn't):

1. **Discoverable** (make *this machine* visible to other devices — `Adapter1.Discoverable` +
   `DiscoverableTimeout`) — dankc only ever *scans*, it never advertises itself.
2. **Remove/unpair a device** (`Adapter1.RemoveDevice(object_path)` or `Device1` object destruction) —
   there is no "forget this device" anywhere in either the control center or Settings.
3. **Trust toggle exposed to the user independent of pairing** — `dc_bluez_pair()` sets `Trusted=true`
   internally as part of the pair flow, but there's no way to view/revoke trust for an
   already-paired device without unpairing entirely.
4. **`RequestPinCode`** (legacy PIN pairing) — currently hard-rejected; fine for "Just Works"/SSP
   modern devices but leaves some old peripherals (some keyboards/mice, era pre-2010) unpairable.
5. **Device type icons** — `dc_bluez_device` has no `Icon`/`Class`/`Appearance` field at all
   (BlueZ's `Device1.Icon` property, e.g. `audio-headset`, `input-keyboard`, `phone`); today every
   device in the list gets the same generic Bluetooth glyph.
6. **Battery level** — BlueZ auto-exposes `org.bluez.Battery1` (`Percentage` property) on a device
   object *automatically* for profiles it can decode battery from (HID, HFP, some LE devices) — no
   extra provider registration needed to *read* it, dankc's `GetManagedObjects` walk just needs to
   also collect this interface's `Percentage` per device (it currently only reads `Device1`/
   `Adapter1` interfaces from that same call and skips everything else via
   `sd_bus_message_skip(reply, "a{sv}")`).
7. **Audio profile / codec hint** — no exposure of `MediaTransport1`'s codec (SBC/AAC/aptX/LDAC) or
   any profile switch (A2DP vs HFP) UI.
8. **Per-device details view** — MAC, class/type, RSSI (`Device1.RSSI`, while visible during a scan),
   UUID/services list, connection state history.
9. **Legacy per-service authorization UI** — `AuthorizeService` is currently auto-allowed
   unconditionally (a deliberate, documented posture matching `bluetoothctl`'s default agent) rather
   than surfaced to the user; fine as a default, but there's no way to change it per-device.

### 2.3 Summary judgment

dankc's foundation is genuinely strong for this: it already talks NetworkManager and BlueZ D-Bus
directly in C via sd-bus (no shelling-out needed for the async pairing chain or the live AP
signal-strength subscription), already has one full custom secret-prompt UI pattern proven out twice
(the Wi-Fi inline password field, and the BlueZ agent confirm/passkey panel reusing that same pattern)
and already ships debug/dry-run hooks that make each new feature screenshot-testable without real
hardware. Most of what's "missing" above is additive within the exact same architecture dankc already
uses, not a new paradigm — this is why the effort estimate in §4 is "extend," not "rewrite."

---

## 3. Recommendation

### Recommendation: **Option B — build native**, with a narrow **Option C escape hatch** for the one
### case genuinely not worth reimplementing (WPA/WPA2/WPA3-Enterprise 802.1x connection setup).

**Why not Option A (reuse Omarchy's tools)**:

- **Backend conflict, not just a style mismatch**: `impala`/`iwgtk` require **iwd**; dankc is built on
  **NetworkManager**. Adopting them means running two competing network stacks or migrating the whole
  system off NM — and per §1.1, that migration is a *downgrade* for exactly the professional feature
  (WPA-Enterprise) this task is about. `bluetui`/`blueman`/`blueberry` don't have this specific
  conflict (BlueZ is BlueZ), but see the next point.
- **Visual cohesion is the actual product**: dankc's entire value proposition versus "just use GNOME"
  is a lightweight, cohesive, Material-themed single-process shell. A TUI in a floating terminal or a
  GTK3 window is a visible seam — the opposite of what a from-scratch C rewrite of DMS is for.
- **Dependency weight**: `blueman` alone pulls in Python3 + PyGObject + dbus-python + its own daemon
  for a feature dankc already has ~70% built in plain C with zero extra runtime. That directly
  contradicts the project's "single core, minimal footprint" premise (see `MEMORY.md`: 99% C,
  niri-only, single core).
- **Omarchy's own justification doesn't transfer**: Omarchy borrows tools because Hyprland has no
  shell of its own — there's nothing more integrated to build toward. dankc already *is* that more-
  integrated shell; reaching for an external tool would be regressing toward Omarchy's situation, not
  learning from it.

**Why Option B is affordable, not just "purer"**: the gap analysis in §2 shows the foundation
(NM/BlueZ D-Bus plumbing, async job state machines, an inline-secret-entry UI pattern, a registered
BlueZ agent) is already built and proven. Nearly every missing feature is a few more D-Bus calls plus
a new inline panel using patterns `controlcenter.c` already has twice over — not new architecture.

**Why keep a small Option C hybrid escape hatch anyway**: WPA/WPA2/WPA3-Enterprise (802.1x: EAP
method selection, CA certificate validation, PEAP/TTLS/TLS inner auth, anonymous identity) is a large
surface with real security stakes (a broken from-scratch 802.1x implementation that silently accepts
bad certs is worse than no implementation). NetworkManager's own `nm-connection-editor` already solves
this correctly and is the *NetworkManager-native* GUI (no backend conflict, unlike impala). A single
"Advanced network settings..." button in dankc's Settings Network tab that launches
`nm-connection-editor` (or `nmtui-edit` as an even lighter fallback if that binary isn't installed) for
the rare enterprise-Wi-Fi case is a pragmatic escape hatch, not a concession of the core recommendation
— it only fires for a feature explicitly marked out-of-scope for native reimplementation, not as the
primary UI.

**Honest effort note**: full 802.1x support, per-connection IP/DNS/routing editing, and a from-scratch
connection-settings object model are each non-trivial (NM's `Settings.Connection` schema is large).
The wave plan below sequences the high-value/low-risk features first (known-networks list, forget,
hidden SSID, adapter discoverable, remove/unpair device, trust toggle, battery %, device icons — all
"more D-Bus calls, same patterns") and defers/escape-hatches the genuinely hard long-tail (802.1x,
full connection editor, VPN) rather than pretending they're equally cheap.

---

## 4. Feature list + D-Bus API map + wave plan

### 4.1 Wi-Fi — NetworkManager D-Bus surfaces needed

| Feature | D-Bus interface / method / property |
|---|---|
| Live scan + signal (**have**) | `org.freedesktop.NetworkManager.Device.Wireless` (`ActiveAccessPoint`, `GetAccessPoints`/`RequestScan`), `org.freedesktop.NetworkManager.AccessPoint` (`Ssid`, `Strength`, `Flags`/`WpaFlags`/`RsnFlags` for security type) |
| Connect open/PSK (**have**, via nmcli) | `org.freedesktop.NetworkManager.AddAndActivateConnection` / `ActivateConnection` (native path, would drop the `nmcli` shell-out) |
| Saved/known connections list | `org.freedesktop.NetworkManager.Settings.ListConnections` + per-connection `GetSettings` on `org.freedesktop.NetworkManager.Settings.Connection` |
| Forget/delete | `Settings.Connection.Delete()` |
| Auto-connect toggle | `Settings.Connection.Update()` rewriting the `connection.autoconnect` key |
| Hidden SSID | `AddAndActivateConnection` with `802-11-wireless.hidden = true` and an explicit `ssid` (can't rely on scan-list click) |
| Security type / WPA2 vs WPA3 vs Enterprise readout | `AccessPoint.WpaFlags`/`RsnFlags` bitmasks (`NM_802_11_AP_SEC_*`); presence of an `802-1x` settings block on a connection = Enterprise |
| Enterprise (802.1x) connect | `Settings.Connection` with an `802-1x` group (`eap`, `identity`, `ca-cert`, `phase2-auth`, ...) — **escape-hatch to `nm-connection-editor`, not natively reimplemented (see §3)** |
| Per-network IP/gateway/DNS/MAC | `org.freedesktop.NetworkManager.Device` (`Ip4Config` → `org.freedesktop.NetworkManager.IP4Config` object's `AddressData`/`Gateway`/`NameserverData`), `Device.HwAddress`/`PermHwAddress` |
| Captive portal hint | `NetworkManager.Connectivity` property (manager-level) + `ActiveConnection.State`/`StateReason` |
| Airplane/rfkill (**have**, partial) | `NetworkManager.WirelessEnabled` (radio-level toggle, replaces the current `nmcli radio wifi`/rfkill shell-outs with a native property write) |
| Wi-Fi password prompts routed correctly | `org.freedesktop.NetworkManager.AgentManager.RegisterWithCapabilities` + dankc exporting an `org.freedesktop.NetworkManager.SecretAgent` object implementing `GetSecrets`/`CancelGetSecrets`/`SaveSecrets`/`DeleteSecrets` — the Wi-Fi analogue of the BlueZ `Agent1` dankc already has |

### 4.2 Bluetooth — BlueZ D-Bus surfaces needed

| Feature | D-Bus interface / method / property |
|---|---|
| Adapter power (**have**) | `org.bluez.Adapter1.Powered` (currently via `bluetoothctl power on/off`; a native `Properties.Set` drops that shell-out) |
| Discoverable | `org.bluez.Adapter1.Discoverable` + `DiscoverableTimeout` |
| Scan (**have**) | `Adapter1.StartDiscovery`/`StopDiscovery` |
| Pair w/ agent (**have**) | `org.bluez.Device1.Pair()` + the already-registered `org.bluez.Agent1` (`RequestConfirmation`/`RequestPasskey`/`RequestAuthorization`; `RequestPinCode` currently rejected, needs a real inline PIN panel for full coverage) |
| Connect/disconnect (**have**) | `Device1.Connect()`/`Disconnect()` (currently via `bluetoothctl`, kept deliberately for its agent-fallback handling — fine to leave as-is) |
| Trust toggle | `Device1.Trusted` (`Properties.Set`) — dankc sets this internally during pairing already; just needs a user-facing read/toggle |
| Remove/unpair | `Adapter1.RemoveDevice(object_path)` |
| Device type icons | `Device1.Icon` (string, e.g. `audio-headset`/`input-keyboard`/`phone`) — map to dankc's existing icon set |
| Battery level | `org.bluez.Battery1.Percentage` — BlueZ auto-exposes this per-device for HID/HFP/some LE profiles once paired+connected; just needs to be read out of the same `GetManagedObjects` walk `scan_objects()` already does (currently skips every interface except `Device1`/`Adapter1`) |
| Audio profile/codec hint | `org.bluez.MediaTransport1.Configuration`/`Codec` (present only while an A2DP/HFP transport is active) |
| Per-device details | `Device1.Address`, `Class`/`Appearance`, `UUIDs`, `RSSI` (scan-time only) |

### 4.3 Wave plan

Each wave is independently shippable and testable via the existing `DANKC_*_DRYRUN`/`DANKC_*_FAKE_*`
debug-hook convention. Sizes: **S** = a few hours (single function + one UI row), **M** = a
half-to-full day (new async job + new panel), **L** = multi-day (new data model / new D-Bus object
export).

**Wave 1 — Bluetooth quick wins (S/M each, no new architecture)**
- `services/bluez.c`: read `Device1.Icon` in `scan_objects()`, map to existing icon set. **S**
- `services/bluez.c`: read `org.bluez.Battery1.Percentage` in the same `GetManagedObjects` walk
  (currently skipped) → `dc_bluez_device.battery_percent`. **S**
- `services/bluez.c` + `ui/controlcenter.c`/`ui/settings.c`: `Adapter1.Discoverable` read + toggle
  (mirrors the existing `Powered` toggle pattern exactly). **S**
- `services/bluez.c`: `Adapter1.RemoveDevice()` (forget/unpair), wired to a new row action /
  long-press or a small "..." menu per device row. **M**
- `services/bluez.c` + UI: expose `Device1.Trusted` as an independent toggle (already set internally
  during pairing — just needs a read + `Properties.Set` write path and a UI control). **S**
- `services/bluez.c`: implement `agent_method_request_pin_code` for real (currently hard-rejects) —
  reuse the exact inline-passkey-entry panel pattern already built for `DC_BLUEZ_AGENT_PASSKEY`. **M**

**Wave 2 — Wi-Fi known-networks list (M/L)**
- `services/net.c`: new `dc_net_saved_connections()` using `Settings.ListConnections` +
  `GetSettings()` per connection (id, autoconnect, last-used timestamp, whether it matches a
  currently-in-range SSID). **M**
- `ui/controlcenter.c` and/or `ui/settings.c`: a "Saved Networks" list separate from the live scan
  rows, each with Forget (`Settings.Connection.Delete()`) and an autoconnect toggle
  (`Update()` on `connection.autoconnect`). **M**
- Hidden SSID entry: a "Connect to hidden network..." affordance opening the same inline
  SSID+password panel as W1.1's flow, feeding `AddAndActivateConnection` with `hidden=true` instead
  of a scan-list click. **M**

**Wave 3 — Wi-Fi detail + native connect path (M/L)**
- `services/net.c`: per-connection detail read (IP/gateway/DNS via the active `IP4Config` object,
  MAC via `Device.HwAddress`) surfaced in a "Network details" panel/Settings sub-view. **M**
- `services/net.c`: replace the `nmcli dev wifi connect` shell-outs (`dc_net_wifi_connect`,
  `dc_net_wifi_connect_psk`) with native `AddAndActivateConnection`/`ActivateConnection` calls,
  keeping the existing async-job/poll state machine shape (`DC_NET_CONNECT_*` enum) — same UI,
  fewer forks, and a real error reason from the D-Bus call reply instead of scraping stdout text
  for "Error". **L**
- Security-type readout (WPA2/WPA3/Open) from `AccessPoint.WpaFlags`/`RsnFlags`, shown per scan row
  and in the detail panel. **S**

**Wave 4 — Wi-Fi secret agent (L)**
- `services/net.c`: export and register a real `org.freedesktop.NetworkManager.SecretAgent` object
  (`AgentManager.RegisterWithCapabilities`) implementing `GetSecrets`/`CancelGetSecrets`/
  `SaveSecrets`/`DeleteSecrets` — directly analogous to the BlueZ `Agent1` already in `bluez.c`
  (`bluez_register_agent()`/the `bluez_agent_vtable`), reusing the same "export vtable, register,
  log+continue on failure" shape. Lets any NM-initiated secret prompt (not just dankc's own inline
  password field) surface in dankc's UI instead of silently failing. **L**

**Wave 5 — Escape hatch + polish (S)**
- `ui/settings.c` `tab_network`: an "Advanced network settings..." button that launches
  `nm-connection-editor` (fallback `nmtui-edit` if not installed) for 802.1x/VPN/static-IP editing —
  explicitly out of scope for native reimplementation per §3. **S**
- Airplane-mode-wide toggle distinct from per-radio Wi-Fi toggle
  (`NetworkManager.WirelessEnabled` + a parallel `NetworkManager.WwanEnabled` if applicable). **S**
- Captive-portal hint surfaced from `NetworkManager.Connectivity`/`ActiveConnection.State`. **S**

Waves 1-3 deliver essentially everything in the "GNOME-Settings-level" feature list except
802.1x-Enterprise and a full connection-settings editor, which Wave 5's escape hatch covers. Wave 4
(the secret agent) is the one piece with no direct precedent to copy verbatim from the codebase (the
BlueZ agent is close but not identical — NM's `GetSecrets` return shape differs from BlueZ's
yes/no/passkey replies) and should be scheduled last, once the rest of the native surface exists to
actually exercise it against.
