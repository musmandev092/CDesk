# dankc Audio Per-Device Management — Plan (2026-07-04)

Ground truth: audio.c = all wpctl subprocess (no libpipewire). Default sink vol/mute = async fork+pipe
(g_fetch, 10s cache, SIGCHLD SIG_IGN); dc_audio_read_source/read_sink_name = sync popen w/ 10s caches.
settings.c tab_audio (L2553) + PRIVATE dup helpers (audio_source_read L904, sinks_read L954 fragile
wpctl-status parse SINKS_MAX 6 no node.name, sinks_set_default L1032). controlcenter.c has THIRD copies
(L848/L895). config string arrays capped 32 chars — too small for node.name (~45). DMS keys by node.name;
deviceMaxVolumes/hiddenOutputDeviceNames/aliases(wireplumber-file+restart).

## Decisions
- D1 ENUM = async `pw-dump` (ships w/ pipewire) → cJSON parse (already linked). One fork gets all nodes:
  node.name/description/media.class(Audio/Sink|Source|Stream/Output|Input/Audio)/application.name/id +
  Props[0].channelVolumes+mute + metadata default.audio.sink/source. GROWABLE malloc buffer (pw-dump
  100KB+, cap 4MiB) like clipboard.c transfer_read. Volume = cbrtf(channelVolumes)*100 (cubic); wpctl
  set-volume takes cubic % directly (only READ converts). Skip *.monitor sources + stream nodes for
  device list. ALL SETS stay wpctl (set-volume/set-mute/set-default <id>) fire-and-forget + stale-kick.
- D2 persistent key = node.name (ids session-local, never persisted).
- D3 aliases = dankc-config-ONLY (dc_audio_display_name lookup covers all dankc surfaces); NOT the
  wireplumber-restart approach (audibly interrupts audio). Document wireplumber variant as follow-up.
- D4 max-volume clamp ENFORCED in service at parse time: vol>max+1 → corrective wpctl set-volume <id>
  <max>% + cache=max (catches slider/mediakeys/external). dc_audio_set_volume clamp 100→default-sink max.
- D5 per-app mixer IN SCOPE (T7 last, droppable): Stream/Output/Audio nodes same pw-dump pass.
- D6 hidden filtered in UI not service (settings needs full list for Unhide).

## API (audio.h): dc_audio_device{id,name[96],desc[64],int volume 0-200,muted,is_default};
dc_audio_sinks/sources(out,max)→count (cached, non-block, stale-kick); dc_audio_device_set_volume(id,pct
clamped)/toggle_mute(id)/set_default(id); dc_audio_display_name(dev)=alias||desc. Existing dc_audio_read/
read_source/read_sink_name keep signatures (main.c OSD tick) but re-backed on the new cache → DELETE the
3 popen paths + both UI files' private copies. T7: dc_audio_stream{id,app,volume,muted}+streams()+stream_set_volume.

## Config (config.h/c): audio_max_volumes[16]{name[96],max_percent 100-200}+n; audio_aliases[16]{name,
alias[48]}+n; audio_hidden[16][96]+n. JSON: audioDeviceMaxVolumes(obj name→int), audioDeviceAliases(obj
name→str), audioHiddenDevices(str array — needs WIDE variant of get/add_string_array, existing is 32ch).
Accessors dc_config_audio_max(name)→100 default / _alias(name)→NULL / _hidden(name)→bool + setters.

## Tasks (Sonnet each). Serialize: audio.c(T1→T3→T7), config.c(T2), settings.c(T4→T7), controlcenter.c(T5).
- T1 audio.c/h: pw-dump async enum + device cache(5s+stale-kick) + sinks/sources + device_set_volume/
  toggle_mute/set_default; re-back read/read_source/read_sink_name; delete old g_fetch+2 popen. DISJOINT.
- T2 config.c/h: 3 tables + object-map get/add helpers + wide string-array + accessors/setters. (‖T1)
- T3 audio.c: parse-time max clamp(1% tol)+corrective fork; set_volume clamps to per-device/default max;
  dc_audio_display_name; read_sink_name returns alias. (after T1,T2)
- T4 settings.c tab_audio: rebuild — OUTPUT/INPUT cards (expand-one, vol slider 0-max, mute, max-vol
  100-200 slider, rename ui_textfield new focus_field, hide btn), HIDDEN section; delete audio_source_read/
  sinks_read/sink_entry/SINKS_MAX/sinks_set_default/g_audio_dirty_until. (after T3)
- T5 controlcenter.c: delete 2 private helpers; alias-aware names; slider max from config; mute via setters. (after T3)
- T6 osd.c/main.c: OSD bar fraction = vol/max; verify main.c diff-poll ok w/ vol>100. (after T3)
- T7 (opt) per-app mixer: streams in pw-dump pass + APPLICATIONS section. (after T3,T4)

## Risks: pw-dump availability(core pkg, safer than wpctl)+size(growable buf+4MiB cap+graceful fail);
cbrtf on read only; node.name key stability(USB port move→stale entries harmless, cap 16 oldest-evict);
clamp feedback loop(1% tol+immediate cache write); async first-paint(empty until ~50-100ms, show hint);
self-invalidate in every setter (replaces g_audio_dirty_until).
