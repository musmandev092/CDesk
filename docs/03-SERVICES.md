# DankC — Backend Services Reference

All D-Bus via **sd-bus** (`<systemd/sd-bus.h>`, or elogind/basu — `05-PORTABILITY.md §2`).
**One system-bus** connection: UPower, PPD, logind, BlueZ, NetworkManager, polkit.
**One session-bus** connection: Notifications(server), MPRIS(client), tray(SNI server).
Audio via **libpipewire**; system stats via **/proc + /sys**; niri via its socket.

## 0. sd-bus essentials

- Connect: `sd_bus_open_system(&bus)` / `sd_bus_open_user(&bus)` (own them; `flush_close_unref` on exit).
- Call: `sd_bus_call_method(bus,dest,path,iface,member,&err,&reply,types,args...)`; parse with
  `sd_bus_message_read`. **Never block the UI** — use `sd_bus_call_method_async(...,cb,userdata,...)`
  for polkit/NM-connect/BlueZ-pair.
- Properties: `sd_bus_get_property{_trivial,_string}`, `sd_bus_set_property(...,"s",val)`.
- Type language: `y b n q i u x t d s o g h`; containers `a`(array) `(...)`(struct) `v`(variant)
  `{...}`(dict entry). Reading `a{sv}`: enter `a`→loop enter `e`("sv")→read key→peek variant sig→enter
  `v`→read/`skip`→exit. **`ay` byte-arrays (NM SSID, tray/notif pixmaps) carry explicit lengths — never
  treat as C strings.**
- Signals: `sd_bus_match_signal(bus,&slot,sender,path,iface,member,cb,ud)`. The universal live-update is
  `org.freedesktop.DBus.Properties.PropertiesChanged` (`sa{sv}as`) — **one generic router keyed by
  (path,interface)** serves UPower/BlueZ/NM/logind/PPD/MPRIS.
- Server side (Notifications/SNI-Watcher/polkit-agent): `sd_bus_add_object_vtable` +
  `sd_bus_request_name`; emit with `sd_bus_emit_signal` / `sd_bus_emit_properties_changed`.
- Loop: poll `sd_bus_get_fd` with `sd_bus_get_events`; `sd_bus_get_timeout` is an **absolute** monotonic
  deadline — convert to relative ms and **round up** (else busy-loop). Drain with `sd_bus_process` until 0.

---

## 1. Audio — PipeWire

Object graph: `pw_init`→`pw_main_loop_new`→`pw_context_connect`→`pw_core_get_registry`. Registry
`global` event per object; enumeration is async (barrier `pw_core_sync` + wait core `done`).

- **Devices:** registry globals with `type==PW_TYPE_INTERFACE_Node`, `PW_KEY_MEDIA_CLASS` ∈
  {`Audio/Sink`,`Audio/Source`} (skip `.monitor`); read `PW_KEY_NODE_NAME/DESCRIPTION`; keep global `id`.
- **Volume:** stored **linearly** in `SPA_PARAM_Props` → `SPA_PROP_channelVolumes` (float[],0..1,
  1.0=unity) + `SPA_PROP_mute` (bool). UI shows **cubic**: `linear = cubic³`, `cubic = ∛linear`.
- **Get/set/watch:** `pw_registry_bind(id,PW_TYPE_INTERFACE_Node,...)`; set via
  `spa_pod_builder_add_object(SPA_TYPE_OBJECT_Props, SPA_PARAM_Props, SPA_PROP_mute, SPA_PROP_channelVolumes...)`
  → `pw_node_set_param`; watch via `pw_node_subscribe_params({SPA_PARAM_Props})`. **Hardware-sink caveat:**
  authoritative volume is on the parent **Device**'s `SPA_PARAM_Route` (nested Props), not the node.
- **Default sink/source:** bind Metadata global `name=="default"`; keys `default.audio.sink`/`.source`
  (transient) and `default.configured.audio.sink`/`.source` (persistent), values JSON `{"name":...}`;
  set via `pw_metadata_set_property`.
- **Recommended hybrid:** shell out to **`wpctl`** for *actions* (`set-volume @DEFAULT_AUDIO_SINK@ 0.5`,
  `set-mute ... toggle`) — it handles cubic + the device-route correctly — and run a small raw-libpipewire
  listener for event-driven *state*. If a GObject dep is acceptable, **libwireplumber** in-process
  (`mixer-api` + `default-nodes-api`) is the cleanest all-in-one (how `wpctl` is built).

## 2. UPower — battery/power (system bus)

Manager `org.freedesktop.UPower` @ `/org/freedesktop/UPower`: `EnumerateDevices()→ao`,
`GetDisplayDevice()→o` (composite, fixed path `.../DisplayDevice` — drive a single indicator from it);
signals `DeviceAdded/Removed(o)`; prop `OnBattery b`.
Device iface `org.freedesktop.UPower.Device`: `Percentage d`(0–100), `State u`
(1 charging/2 discharging/3 empty/4 full/5 pending-charge/6 pending-discharge), `TimeToEmpty x`,
`TimeToFull x`, `IconName s`, `Type u`, `IsPresent b`, `Capacity d`(health). **No per-prop signals** →
subscribe `PropertiesChanged` on the device path(s) + Manager `DeviceAdded/Removed`.

## 3. power-profiles-daemon (system bus)

Prefer `org.freedesktop.UPower.PowerProfiles` @ `/org/freedesktop/UPower/PowerProfiles`; **fall back** to
legacy `net.hadess.PowerProfiles` if unowned. Props: `ActiveProfile s` (RW: power-saver|balanced|
performance), `Profiles aa{sv}` (each has `Profile`,`Driver`), `PerformanceDegraded s`. Set via
`Properties.Set(...ActiveProfile...)`. `HoldProfile(sss)→u`, `ReleaseProfile(u)`. Watch `PropertiesChanged`.

## 4. logind — org.freedesktop.login1 (system bus)

Manager @ `/org/freedesktop/login1` (iface `...Manager`):
- **Brightness:** `SetBrightness(s subsystem, s name, u value)` — subsystem "backlight", name kernel
  device (`intel_backlight`, `amdgpu_bl0`), value raw 0..max (read `/sys/class/backlight/<name>/max_brightness`).
  No signal → reflect external changes via udev/inotify on sysfs. Send no-reply for reactive sliders.
- **Power:** `Suspend(b)`, `Reboot(b)`, `PowerOff(b)`, `Hibernate(b)`, `SuspendThenHibernate(b)`
  (bool=interactive; pass false).
- **Lock:** `LockSession(s)`, `UnlockSession(s)`, `LockSessions()`.
- **Idle inhibitor:** `Inhibit(s what, s who, s why, s mode)→h fd` — keep-awake = `what="idle",
  mode="block"`; **hold the fd, close to release**.
- Signals: `PrepareForSleep(b)` (before/after suspend), `PrepareForShutdown(b)`.
Session @ `/org/freedesktop/login1/session/self`: `Lock()`/`Unlock()` signals (lock screen subscribes),
`SetLockedHint(b)`, `SetIdleHint(b)`; props `LockedHint`,`IdleHint`,`Active`.

## 5. BlueZ — org.bluez (system bus)

Root ObjectManager @ `/`: `GetManagedObjects()→a{oa{sa{sv}}}` (adapters=`org.bluez.Adapter1`,
devices=`org.bluez.Device1`, battery=`org.bluez.Battery1.Percentage y`). Watch ObjectManager
`InterfacesAdded/Removed` + per-object `PropertiesChanged`.
- **Adapter1** (`/org/bluez/hciX`): `StartDiscovery()`, `StopDiscovery()`, `RemoveDevice(o)`,
  `SetDiscoveryFilter(a{sv})`; props `Powered b`(RW), `Discovering b`, `Discoverable b`, `Alias s`,
  `PowerState s` (on/off/off-enabling/on-disabling — transitional UI).
- **Device1**: `Connect()`, `Disconnect()`, `Pair()`, `CancelPairing()`; props `Connected b`,`Paired b`,
  `Trusted b`(RW), `Name s`,`Alias s`,`Address s`,`Icon s`,`RSSI n`,`UUIDs as`.
- **Pairing agent** (optional): export `org.bluez.Agent1`, `AgentManager1.RegisterAgent(o,"KeyboardDisplay")`
  + `RequestDefaultAgent(o)`; handle `RequestConfirmation`,`RequestPasskey`,`DisplayPasskey`,`AuthorizeService`.

## 6. NetworkManager via sd-bus, no libnm (system bus)

Root `org.freedesktop.NetworkManager` @ `/org/freedesktop/NetworkManager`: `GetDevices()→ao`,
`ActivateConnection(o,o,o)→o`, `AddAndActivateConnection(a{sa{sv}},o,o)→(o,o)`, `DeactivateConnection(o)`;
props `Devices ao`,`ActiveConnections ao`,`PrimaryConnection o`,`WirelessEnabled b`(RW),`State u`;
signals `StateChanged(u)`,`DeviceAdded/Removed(o)`.
- **Device** `...Device`: `State u`,`DeviceType u`(Wi-Fi=2,Ethernet=1),`Interface s`,`ActiveConnection o`;
  `Disconnect()`; signal `StateChanged(u,u,u)`.
- **Wi-Fi** `...Device.Wireless`: `RequestScan(a{sv})`, `GetAllAccessPoints()→ao`; props `AccessPoints ao`,
  `ActiveAccessPoint o`,`LastScan x`. Scan = RequestScan → wait `LastScan` change → re-read APs.
- **AccessPoint** `...AccessPoint` (RO): `Ssid ay`(**raw bytes+len**), `Strength y`(0–100),
  `Flags u`(bit0 privacy), `WpaFlags u`, `RsnFlags u` (nonzero ⇒ secured), `Frequency u`, `HwAddress s`.
- **Active** `...Connection.Active`: `State u`(2=ACTIVATED),`Id s`,`Uuid s`,`Type s`; `StateChanged(u,u)`.
- **Settings** @ `.../Settings`: `ListConnections()→ao`, `AddConnection(a{sa{sv}})→o`; per-profile
  `GetSettings`,`GetSecrets`,`Update`,`Delete`.
- **Connect to Wi-Fi:** `AddAndActivateConnection(conn, wifi_dev, ap)`; conn `a{sa{sv}}` =
  `connection{type:"802-11-wireless",id}` + `802-11-wireless{ssid:<ay>,mode:"infrastructure"}` +
  `802-11-wireless-security{key-mgmt:"wpa-psk",psk}` (omit for open) + `ipv4{method:"auto"}` +
  `ipv6{method:"auto"}`. Build with nested `sd_bus_message_open_container`.
- Watch `PropertiesChanged` per object + the three `StateChanged` + `DeviceAdded/Removed`.

## 7. Notifications SERVER — we are the daemon (session bus)

Own `org.freedesktop.Notifications` @ `/org/freedesktop/Notifications`.
| Method | in | out |
|---|---|---|
| `Notify` | `susssasa{sv}i` (app_name,replaces_id,app_icon,summary,body,actions,hints,expire_timeout) | `u` id |
| `CloseNotification` | `u` | — |
| `GetCapabilities` | — | `as` |
| `GetServerInformation` | — | `ssss` |
`replaces_id≠0` updates that id (return same); `0`→new nonzero id. `expire_timeout`: -1 server-decides,
0 never, >0 ms. `actions`=flat `[key,label,...]` (`"default"`=click). Signals: `NotificationClosed(u,u)`
(reason 1 expired/2 dismissed/3 CloseNotification/4 undefined), `ActionInvoked(u,s)`,
`ActivationToken(u,s)` (emit before ActionInvoked). Hints to parse: `urgency y`(0/1/2), `category s`,
`desktop-entry s`, `image-path s`, `image-data (iiibiiay)` (RGBA row-major), `sound-file/sound-name s`,
`suppress-sound b`, `transient b`, `resident b`, `action-icons b`. Advertise capabilities you implement:
`actions,action-icons,body,body-markup,body-hyperlinks,body-images,icon-static,persistence,sound`.
(Behavioral policy — dedupe, rate-limit, timeouts, history — in `04-FEATURES.md §notifications`.)

## 8. MPRIS2 client (session bus)

Discovery: `ListNames` filter prefix `org.mpris.MediaPlayer2.`; watch `NameOwnerChanged` with
`arg0namespace='org.mpris.MediaPlayer2'`. Each player @ `/org/mpris/MediaPlayer2`.
`org.mpris.MediaPlayer2.Player`: `PlayPause/Play/Pause/Stop/Next/Previous`, `Seek(x)`,
`SetPosition(o,x)`; props `PlaybackStatus s`, `Metadata a{sv}`, `Position x`(µs), `Volume d`,
`CanGoNext/…`,`LoopStatus`,`Shuffle`; signal `Seeked(x)`. Metadata keys: `mpris:trackid o`,
`mpris:length x`, `mpris:artUrl s`, `xesam:title s`, `xesam:artist as`(**array**), `xesam:album s`.
Watch `PropertiesChanged` (iface Player). **`Position` is NOT in PropertiesChanged** — poll `Get(Position)`
on a timer + use `Seeked`.

## 9. System tray — StatusNotifierItem/Watcher (session bus, we implement Watcher+Host)

- **Watcher:** own `org.kde.StatusNotifierWatcher` @ `/StatusNotifierWatcher`:
  `RegisterStatusNotifierItem(s)` (bus name or `/path`; resolve to `"busname/path"`; emit
  `StatusNotifierItemRegistered`), `RegisterStatusNotifierHost(s)`; props `RegisteredStatusNotifierItems as`,
  `IsStatusNotifierHostRegistered b`, `ProtocolVersion i`; signals Item/Host Registered/Unregistered.
  Watch `NameOwnerChanged` → drop items whose owner died.
- **Host:** own `org.kde.StatusNotifierHost-<PID>`, call `RegisterStatusNotifierHost`, enumerate items.
- **Item** `org.kde.StatusNotifierItem` (remote): props `Category`,`Id`,`Title`,`Status`(Passive/Active/
  NeedsAttention),`IconName s`,`IconPixmap a(iiay)`(ARGB32 net byte order),`ToolTip`,`ItemIsMenu b`,`Menu o`;
  methods `Activate(i,i)`(left), `SecondaryActivate(i,i)`(middle), `ContextMenu(i,i)`, `Scroll(i,s)`;
  signals `NewIcon/NewToolTip/NewTitle/NewStatus/…` (re-fetch prop).
- **Menu** `com.canonical.dbusmenu` @ Item's `Menu`: `GetLayout(i,i,as)→(u,(ia{sv}av))`,
  `Event(i,s,v,u)` (clicked/hovered/opened/closed), `AboutToShow(i)→b`; props `label`,`enabled`,`visible`,
  `type`,`toggle-type`,`toggle-state`,`icon-name`,`children-display`.

## 10. polkit agent + PAM (system bus)

Authority `org.freedesktop.PolicyKit1` @ `/org/freedesktop/PolicyKit1/Authority`:
`RegisterAuthenticationAgent((sa{sv}) subject,"", o agent_path)` (subject `"unix-session"`
`{session-id:<XDG_SESSION_ID>}`), `AuthenticationAgentResponse2(u uid,s cookie,(sa{sv}) identity)`.
Export `org.freedesktop.PolicyKit1.AuthenticationAgent`:
`BeginAuthentication(s action_id,s message,s icon,a{ss} details,s cookie,a(sa{sv}) identities)` and
`CancelAuthentication(s cookie)`.
**PAM approach (don't setuid our backend):** on BeginAuthentication spawn
`/usr/lib/polkit-1/polkit-agent-helper-1 <username>`, write `cookie` to its **stdin**, feed the password
(from our Wayland dialog) on `PAM_PROMPT_ECHO_OFF`; on SUCCESS the helper already ran PAM **and** sent
`AuthenticationAgentResponse2`, so just return. Or link `libpolkit-agent-1`
(`PolkitAgentListener`) and only render UI. Lock-screen auth uses the same helper/`pam_start("...")` path.

## 11. System monitor — /proc + /sys (replaces dgop)

Pure file reads, near-zero cost, no dgop dependency:
- **CPU:** `/proc/stat` (per-core jiffies; %= delta busy / delta total between ticks).
- **RAM:** `/proc/meminfo` (`MemTotal`,`MemAvailable`).
- **Temps:** `/sys/class/hwmon/hwmon*/temp*_input` (label via `.../name` + `temp*_label`); CPU pkg,
  GPU (amdgpu/nvidia) here too.
- **Disk:** `statvfs()` per mount for usage; `/proc/diskstats` for IO.
- **Net:** `/proc/net/dev` (rx/tx bytes → rate between ticks).
- **GPU:** amdgpu/intel via hwmon/sysfs; nvidia via NVML (optional). Poll on a `timerfd` (e.g. 2 s).

## 12. niri IPC

Socket = env **`NIRI_SOCKET`** (unset ⇒ not under niri; never hardcode), UNIX SOCK_STREAM,
**newline-delimited JSON**, one reply line per request: `{"Ok":<Response>}` or `{"Err":"..."}`.
Use **one connection per one-shot request**; `EventStream` keeps a dedicated connection.
- **Requests:** unit variants = bare quoted string (`"Outputs"`,`"Workspaces"`,`"Windows"`,
  `"FocusedWindow"`,`"EventStream"`,`"KeyboardLayouts"`); struct variants = single-key object
  (`{"Action":{"FocusWorkspace":{"reference":{"Index":2}}}}`, `{"Action":{"CloseWindow":{"id":12}}}`,
  `{"Output":{"output":"DP-1","action":{"On":{}}}}`).
- **Types:** Workspace `{id u64, idx u8, name?, output?, is_urgent, is_active, is_focused,
  active_window_id?}`; Window `{id, title?, app_id?, pid?, workspace_id?, is_focused, is_floating,
  is_urgent, layout}`; Output map keyed by name `{make,model,modes[],current_mode?,logical{x,y,width,
  height,scale,transform},vrr_*}`.
- **EventStream:** send `"EventStream"` → first `{"Ok":{"Handled":null}}` (discard) → one Event JSON per
  line. On connect: full-state burst (`WorkspacesChanged`,`WindowsChanged`,…) then deltas
  (`WorkspaceActivated`,`WindowOpenedOrChanged`,`WindowClosed`,`WindowFocusChanged`,
  `OverviewOpenedOrClosed`,`KeyboardLayoutSwitched`,…). **Plural = snapshot, singular = delta.** Keep
  local maps keyed by `id`.
- **Versioning:** protocol tracks the niri release, **not semver** — **ignore unknown fields/variants**.
  Verify schema against the running niri via `niri msg --json <cmd>` (never parse non-JSON output).
- Config/keybind generation into `~/.config/niri/dms/*.kdl`: `04-FEATURES.md §niri`.
