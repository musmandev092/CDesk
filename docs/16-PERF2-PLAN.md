# DankC — Performance Plan 2: Everything Except Memory (P9)

docs/15-PERF-PLAN.md put memory at its practical floor (~20 MB Private,
~39 MB Pss; the rest of RSS is Mesa pages niri holds resident regardless of
dankc — do not re-audit memory). This doc covers every *other* performance
dimension: startup time, idle CPU/wakeups, panel-open latency, frame pacing,
input latency, subprocess forks, render/damage efficiency, main-loop
blocking, build flags, and hot-path profiling. **Analysis only** — no
production code changed; this doc plans work for other agents/sessions.

Method: no `strace`/`bpftrace`/`valgrind` on this host, and `perf` is
present but blocked by `perf_event_paranoid=2` with no BPF (same constraint
docs/15 hit). Instead: temporary `clock_gettime()` traces were added at
every startup phase boundary, in the event loop's fd-dispatch (per-fd fire
counts over 30 s windows), and around a panel's create→first-frame path;
measured on the **live niri session** (own PID, `WAYLAND_DISPLAY=wayland-1`,
`niri` PID 1048 — the real desktop, not nested), 2 real outputs, `-O2 -g`
build @ `de5a0c7`. All instrumentation was reverted before writing this doc
(`git status` is clean; only this file is new).

---

## 1. Startup time / time-to-first-frame — MEASURED

Two regimes matter and differ by ~3×: **cold** (page cache empty for Mesa's
`.so`s and the font files — a fresh login) and **warm** (a shell restart
during an active session, or Mesa/fonts already paged in by another GL/text
client — e.g. DMS `qs` already running). Both were measured directly (3
runs each, `main()` entry to first `eglSwapBuffers`):

| Phase | Cold (1st run) | Warm (2nd–3rd run, page cache hot) |
|---|---:|---:|
| theme_init + config_load + wayland_connect | 1.7 ms | 0.8 ms |
| **`eglInitialize`/`eglCreateContext` (EGL/Mesa driver load)** | **87.9 ms** | **~25 ms** |
| niri_connect + dbus_connect + bluez/net/mpris/power/notifications/tray init | ~5.3 ms | ~2.7 ms |
| **`dc_autostart_run()`** (scans + fork/execs 4 XDG autostart apps) | **24.5 ms** | **~5.2 ms** |
| bar_create + all panel `_create()` calls | 7.8 ms | 3.4 ms |
| wait for first layer-surface `configure` | ~3.9 ms | ~3 ms |
| first `dc_bar_render()` → first `eglSwapBuffers` (total) | **184.3 ms** | **~61 ms** |
| — of which: `dc_render_ensure()` (nanovg ctx + 8 font loads + cmap parses) | 108.0 ms | ~35 ms |
| — of which: **not otherwise accounted (see §1.3 below)** | ~76 ms | ~26–40 ms |
| **Total: `main()` → first pixel on screen** | **~315 ms** | **~101 ms** |

### 1.1 EGL/Mesa init (88 ms cold / ~25 ms warm) — mostly not dankc's to fix

`eglGetPlatformDisplay`+`eglInitialize` dlopen the Mesa megadriver
(`libgallium-*.so`, pulling in `libLLVM.so`, per docs/15 — the same libraries
already confirmed shared with niri). The **cold/warm gap (88→25 ms) is
almost entirely page-cache warmth**, not dankc's code — verified by running
back-to-back after a `make clean` rebuild (guaranteed cold) vs. immediately
re-launching (guaranteed warm), same binary, same flags. This is a real cost
but not attributable to application logic; docs/15 §2–3 already rejected
Vulkan/EGL-device tricks as ways around it (niri needs the same driver
regardless). **Do not re-litigate; the actionable move is hiding it (see
Wave 1 T1.1: run it concurrent with independent init work), not shrinking
it.**

### 1.2 Font loading (108 ms cold / ~35 ms warm) — CJK/Devanagari confirmed the biggest slice

Timestamped every one of the 8 `nvgCreateFontMemAtIndex` + cmap-parse calls
inside `dc_render_ensure()` (cold run): UI (Inter) + icons ≈ 1 ms combined
(mmap already done per docs/15 T1.1 — this is cmap-parse time only), then
fallback cmap parses: **NotoSans 29 ms, NotoSansArabic 2 ms, NotoSansCJK.ttc
33 ms, NotoSansDevanagari 10 ms, NotoSansThai 2 ms, NotoEmoji 3 ms.**

For this shell's actual configured user (English UI + Urdu/Arabic
fallback — see memory `language-urdu-in-english-out`), **CJK (33 ms) +
Devanagari (10 ms) + Emoji (3 ms) = 46 ms, ~43% of font-load time, are spent
parsing scripts that are unlikely to ever render.** This is exactly
docs/15's already-identified T2.1 ("lazy fallback-font loading"), now with
real timing data justifying it as a **startup-time** win, not just a memory
one. NotoSans (Cyrillic/Greek, 29 ms) is a closer call — cheap to keep
eager, but is also a defer candidate.

### 1.3 The unaccounted ~76 ms (cold) — a synchronous `popen()` sits ahead of the first frame

`dc_bar_render()` computes `bar_compute_signature()` **before** doing any
EGL/font work (`src/ui/bar/bar.c:2193`, ahead of the `dc_render_ensure()`
call at :2220). That signature hash reads `dc_audio_read()`
(`src/services/audio.c`), which — on the very first call, cache empty —
does a **synchronous `popen("wpctl get-volume ...")` + blocking read +
`pclose()`**. Standalone-timed on this host: **`wpctl get-volume` takes
35–37 ms per invocation** (fork + exec + PipeWire round-trip). This fork
happens to land squarely between "first `dc_bar_render()` entered" and font
loading starting, and is the largest single piece of the unaccounted gap.
The remainder is first-frame GL setup (`eglMakeCurrent`, `wl_egl_window`
alloc, `glClear`, the actual nanovg draw calls, text shaping for the first
paint) — normal one-time cost, not blocking-fork related.

**This popen is not a one-time startup cost — it recurs every ~10 s for the
life of the process** (see §6): every recurrence blocks the *entire*
single-threaded event loop — rendering, input, animation — for ~35–40 ms.
This is the single highest-value, lowest-risk finding in this whole audit
(see Wave 1 T1.2).

### 1.4 `dc_autostart_run()` blocks bar creation for no reason

`main()` calls `dc_autostart_run()` (scans `~/.config/autostart` +
`/etc/xdg/autostart`, forks 4 apps on this desktop: nm-applet,
print-applet, xdg-user-dirs-update, xapp-sn-watcher) **before** the bar
creation loop (`src/main.c`, autostart line precedes `dc_bar_create()`).
Autostart is fire-and-forget spawning of *other* processes — it has zero
dependency on the bar being up, and its own COW-fork cost (24.5 ms cold /
~5 ms warm for 4 forks) directly delays first paint for no reason.

---

## 2. Idle CPU + wakeups — MEASURED, confirms docs/15's numbers with a cause

- **CPU:** 3 ticks / 20 s (utime+stime, `CLK_TCK=100`) = **0.15%**, matching
  docs/15's 0.08–0.2%. Confirmed floor, not re-litigated.
- **Loop's own `poll()` wakeups:** instrumented every fd slot's fire count
  over 30 s windows. Result: **~1–4.5 poll() returns/sec**, overwhelmingly
  one fd (the Wayland display socket, confirmed via `/proc/<pid>/fd`
  `readlink`) — 0.7–3.4 fires/sec correlating with desktop activity (pointer
  motion over dankc's own surfaces, focus/output events), not a fixed
  internal timer. D-Bus fds and the OSD timerfd fired **≤0.16/sec each**
  (near-silent). The deliberate 1 Hz `clock_tick` fires exactly once/sec as
  configured (confirmed: `dc_loop_set_tick(..., 1000)` in `main.c`, and the
  render-stats log shows ~58–59 `dc_bar_render()` calls/60 s across 2 bars).
- **`voluntary_ctxt_switches` (whole-process, `/proc/<pid>/status`):**
  ~22–23/sec — **much higher than the loop's own ~1–4.5 poll() wakeups/sec**.
  The gap is the other 4 threads (Mesa `iris` gallium util threads:
  `disk$0`, `sh0`, `traceq0`, `gdrv0` per docs/15) doing their own
  futex/scheduling churn, independent of dankc's event loop. **Not
  attributable to application code** — same "Mesa owns it regardless"
  conclusion as the memory audit.
- **Clock tick granularity confirmed as suspected:** `dc_loop_set_tick(g_loop,
  clock_tick, &tick, 1000)` ticks every 1000 ms unconditionally, even though
  the bar shows `HH:MM` (no seconds) by default (`bar.c`:
  `cfg->show_seconds` gates `%H:%M:%S` vs `%H:%M`). The tick is not wasteful
  in isolation — `bar_compute_signature()`'s damage check means 49–55 of
  every 58–59 ticks/minute skip the actual GL draw (render-stats:
  "58 calls, 3 drawn, 55 skipped" / "59 calls, 10 drawn, 49 skipped") — but
  the same tick also drives `dc_sysmon_poll()`, `dc_processes_refresh()`,
  and `dc_dashboard_refresh()`, i.e. it is deliberately multi-purpose, not
  purely clock-driven. **Low priority**: a minute-granularity clock ticking
  once/sec is technically 60× more often than needed for its own display
  purpose, but the measured cost of doing so is already at noise floor.

## 3. Panel-open latency — MEASURED, already fast

Instrumented control-center's `cc_show()` (surface commit) →
`eglSwapBuffers` for a cold-vs-warm open on the running process (bar
already up, shared `EGLContext`/`NVGcontext` per docs/15 §3.2):

- **Cold open (first ever CC show): 30.3 ms** from surface-commit to first
  visible frame — fast, because the render context/fonts are already warm
  from the bar's own init. This is essentially just `wl_egl_window_create` +
  `eglCreatePlatformWindowSurface` (per-panel, created-on-show, torn down on
  hide — docs/15 confirmed no per-panel duplicate atlas/context) + one
  nanovg frame.
- Not a bottleneck at ~2 frames' worth of latency (60 Hz frame = 16.6 ms).
  **No pre-warming/pooling needed** — the existing lazy create-on-show
  design (docs/15 §a) is already close to optimal, confirmed with real
  numbers this pass instead of just code-reading.
- Minor, low-confidence observation: the first 2–3 animation frames of a
  panel-open swap only ~2 ms apart (vs. an expected ~16 ms vsync cadence).
  Plausible explanation: a not-yet-mapped/composited surface's frame
  `done` callback can fire immediately (common benign Wayland behavior —
  the compositor has nothing to wait for before the surface is actually on
  screen). Not chased further this pass; flagged as Wave 3 investigation
  only if animation jank is ever visually reported.

## 4. Frame pacing / animation smoothness — verified correct, no changes needed

- `eglSwapInterval(dpy, 0)` + frame-callback gating (re-arm `wl_surface_frame`
  only while `dc_anim_active()`/marquee/closing) matches docs/02-RENDERING
  §2's documented pattern, confirmed by reading `bar.c`/`controlcenter.c` —
  no idle surface holds a live frame callback.
  `bar_compute_signature()`'s `anim_in_flight` bypass correctly forces a
  redraw during workspace-morph/marquee even when the hash is otherwise
  unchanged (`bar.c:2194`).
- Damage tracking (docs/15 P7) verified live: render-stats logs show
  **83–92% of ticks skip the GL draw entirely** at idle. No full-surface
  redraw-when-nothing-changed bug found.
- No busy-wait found anywhere in the render or animation path.

## 5. Input latency — no blocking found on the direct click/key path, one indirect risk

- `handle_bar_click`/`handle_key` dispatch synchronously into UI code with
  no `popen`/`system`/blocking I/O directly in the call chain for hover,
  click-routing, or keyboard input.
- User-initiated actions that touch external state (volume slider drag →
  `dc_audio_set_volume()`, workspace click → `dc_niri_focus_workspace()`,
  mute toggle, bluetooth pair, etc.) all use **fire-and-forget `fork()`**
  (non-blocking — parent returns immediately, reaped via `SIGCHLD=SIG_IGN`).
  These do not stall input.
- **Indirect risk**: any click that triggers a **read** of current audio
  state (e.g. opening the control center right as the bar's 10 s audio
  cache expires) can land inside the ~35–40 ms `wpctl` `popen()` block from
  §1.3/§6, stalling that click's visible response by the same amount. Same
  root cause as the startup finding — fixing it (Wave 1 T1.2) fixes both.

## 6. Remaining subprocess forks — MEASURED: exactly one recurring offender

Confirmed via a clean single-instance idle run (10-minute log): **the audio
`wpctl get-volume` popen fires every ~10.1 s**, matching the configured
`DC_AUDIO_CACHE_SECONDS=10` (docs/15 T2.3) — the cache itself works
correctly. What docs/15 didn't measure is the **per-fork cost**: standalone-
timed, `wpctl get-volume` takes **35–37 ms wall-clock** (fork + exec +
dynamic link + PipeWire connect + query + exit) — **and this is a
synchronous, main-thread-blocking `popen()`/`pclose()` pair**, not a
background/async call. At ~6 forks/min this is ~210–222 ms/min of total
main-loop stall time, arriving in ~35–40 ms chunks that each freeze
rendering/input.

- Wi-Fi (net.c): confirmed **event-driven D-Bus**, 0 background forks
  (docs/15 T2.2, verified still true — no `nmcli` calls seen in a 10-minute
  idle log).
- Bluetooth (bluez.c), MPRIS (mpris.c), battery (battery.c), weather
  (weather.c), sysmon (sysmon.c): confirmed no `popen`/`system`/`fork` in
  their hot-path read functions (`dc_bluez_read`, `dc_mpris_read`,
  `dc_battery_read`, `dc_weather_get`, sysmon's `/proc` reads) — all
  D-Bus/sysfs/procfs, no subprocess.
- Autostart forks (4, `main.c`) are one-shot at startup, not recurring —
  already covered in §1.4 as a startup-latency issue, not a forever-forking
  one. (Side note, out of scope for this perf pass: repeated dankc restarts
  during this audit visibly re-spawned duplicate nm-applet/xapp-sn-watcher
  instances each time — a correctness nit for whoever owns autostart
  dedup, not a dankc perf issue.)
- **Conclusion: audio.c's `wpctl` popen is the only remaining
  poll-driven fork**, and the fix is not "poll less" (already at a
  reasonable 10 s) but "don't block the main thread while forking" (Wave 1
  T1.2).

## 7. Render efficiency / damage — already good (P7), one minor observation

Covered in §4: damage tracking works, ~85–90% of ticks skip GL work. One
architectural (not measured-costly) observation: `bar_compute_signature()`
unconditionally re-derives *everything* (strftime, 5+ service reads,
tray/notification state, hover) every tick to decide if anything changed,
rather than services pushing "I changed" signals. Measured cost of this
recompute is sub-millisecond (doesn't move the 0.15% CPU number) — **not
worth restructuring**, noted for completeness only.

## 8. Blocking on the main loop — the headline finding, see §1.3/§5/§6

The synchronous `wpctl get-volume` `popen()` in `src/services/audio.c`
(`dc_audio_read()`) is the only confirmed main-loop-blocking call, but it is
a real one: ~35–40 ms of total UI freeze every ~10 s, and it sits directly
in the first-frame startup path too. No synchronous `sd_bus_call` (vs.
async) was found — `dbus.c`'s integration is fd/event-driven throughout the
services checked. No other blocking file reads found (sysfs/procfs reads
are all small, single-`read()`, non-blocking-in-practice).

## 9. Build/link flags — MEASURED: -O3 gives no measurable benefit; reject LTO effort

Rebuilt the entire tree with `-O3` (CFLAGS, CXXFLAGS, and `TP_CFLAGS` for
the vendored nanovg/cJSON, which is where the hot cmap-parsing/font-loading
code lives) and re-ran the same startup trace 3× warm-cache, comparing
against 3× warm-cache `-O2` runs (both after `make clean` + full rebuild,
same binary flags otherwise):

| | `-O2` (warm, 3 runs) | `-O3` (warm, 2 comparable runs) |
|---|---:|---:|
| `eglInitialize`/`eglCreateContext` | 24.6–25.3 ms | 24.4–28.6 ms |
| `dc_render_ensure()` (fonts+cmaps) | 31.5–43.7 ms | 32.5–38.9 ms |

**Statistically indistinguishable** — within the same run-to-run noise
band. This makes sense in hindsight: the two dominant startup costs are
(a) `dlopen`-ing a **precompiled system Mesa driver** dankc's own
`-O2`/`-O3` flag can't touch at all, and (b) cmap/font-table parsing, which
is a sequential byte-walk over mmap'd pages (I/O/cache-fault bound, not a
tight numeric loop `-O3`'s vectorizer/inliner would speed up). **Verdict:
reject `-O3`/LTO as a startup-latency lever** — do not spend effort here;
this directly contradicts the "obviously LTO helps hot paths" intuition,
measured not assumed, same spirit as docs/15's malloc-allocator rejections.
(LTO could still be revisited *if* a real CPU-bound hot loop is found via
better profiling tools later — none was found this pass.) Binary is
currently 4.03 MB `-O2 -g`, not stripped for dev builds; docs/15 T1.3
already covers the install-time strip (0 MB RAM impact, disk-only).

## 10. Hot-path profiling — no tool access; reasoning + direct instrumentation used instead

Same tooling gap as docs/15 (`perf_event_paranoid=2`, no BPF, no
`strace`/`valgrind`/`ltrace` installed). Compensated with direct
`clock_gettime()` bracketing (§1–§9 above) rather than sampling. Conclusion:
no per-frame algorithmic waste found in steady state — the render path is
damage-gated (§4/§7) and idle CPU is at noise floor (§2). The only real
"hot bursts" are one-time startup costs (EGL/Mesa dlopen, font/cmap
parsing) already covered above, neither of which responds to compiler
flags (§9).

---

## Ranked backlog — waves (disjoint files, parallelizable within a wave)

### Wave 1 — high impact, low risk, SAFE-TO-IMPLEMENT-NOW

**T1.1 — parallelize EGL init with niri/dbus/service-init/autostart**
- Measured cost: EGL init (25–88 ms) fully serializes in front of
  niri_connect+dbus_connect+bluez/net/mpris/power init+notifications/tray
  create+autostart (8–30 ms combined) — the latter fits entirely inside the
  former's window.
- Change: spawn a thread (or defer via a one-shot idle callback if a thread
  is undesired) to run `dc_egl_init()` while the main thread proceeds with
  `dc_niri_connect()`/`dc_dbus_connect()`/service inits/autostart scan (all
  independent of EGL — none touch `egl`/`render` state); `pthread_join`
  (or poll-for-done) right before the bar-creation loop, which is the first
  consumer of `egl`.
- Files: `src/main.c` only.
- Expected win: **hides ~8–30 ms of serialized init inside the EGL window
  (bigger % win on the cold-cache 88 ms case)**; near-zero risk since the
  parallel work touches disjoint state.
- Effort: **S**. Risk: **Low** (one join point, no shared mutable state
  during the parallel section).

**T1.2 — make `dc_audio_read()`'s `wpctl` call non-blocking** ⭐ biggest win, touches responsiveness AND startup AND forks
- Measured cost: synchronous `popen()`+`pclose()` blocks the single-
  threaded main loop for **35–40 ms**, once at first render (directly in
  the startup critical path, §1.3) and every ~10 s thereafter for the life
  of the process (§6/§8) — freezing rendering/input/animation each time.
- Change: replace the blocking `popen()`/`fgets()`/`pclose()` sequence with
  a non-blocking child: `fork()`+`exec()` "wpctl get-volume" with stdout
  piped to a fd registered on `dc_loop_add_fd()` (POLLIN), parse the line
  when it arrives, update the cache from the fd callback instead of inline.
  `dc_audio_read()` itself becomes: return the (possibly momentarily stale)
  cache immediately, and if a refresh isn't already in flight and the cache
  is stale, kick one off async. First-ever call (cache empty, `available =
  false`) no longer blocks anything — the bar just shows "no reading yet"
  for one tick until the async result lands, matching how `dc_weather_get()`
  already behaves (docs/02 pattern: read cached, refresh out-of-band).
- Files: `src/services/audio.c` (+ its existing `dc_loop`-taking init, if
  it needs the loop handle passed in from `main.c` — check `dc_audio_init()`
  signature/callers before starting).
- Expected win: **removes ~35–40 ms of UI freeze every ~10 s** (idle
  responsiveness) **and ~35–40 ms off both cold and warm startup**
  (first-render no longer blocks on it). Fork *count* is unchanged (still
  ~6/min) — this fixes blocking, not frequency.
- Effort: **M** (event-driven subprocess plumbing, mirrors the pattern
  `clipboard.c`'s `transfer_read` fd callback already uses in this
  codebase — reuse that shape). Risk: **Low-Medium** (must handle the
  child exiting/pipe EOF cleanly; don't leak fds/zombies — reuse the
  existing `SIGCHLD=SIG_IGN` reaping).

**T1.3 — defer `dc_autostart_run()` until after the first frame is on screen**
- Measured cost: 24.5 ms cold / ~5 ms warm, unconditionally serialized
  before bar creation, for work that has zero dependency on the bar being
  up (fire-and-forget spawning of other desktop apps).
- Change: move the `dc_autostart_run()` call from before the bar-creation
  loop to either (a) right after `dc_loop_run()` is entered, via a one-shot
  timer/idle callback fired on the first tick, or (b) simplest: just move
  the call to after all panel `_create()` calls and the "entering event
  loop" log, still before `dc_loop_run()` — this alone removes it from the
  critical path up to bar creation even without deferring past first paint.
  Prefer (a) if a trivial "run once after loop starts" hook already exists
  (check `dc_loop_add_prepare`/tick mechanism first); otherwise (b) is a
  1-line reorder with the same safety.
- Files: `src/main.c` only.
- Expected win: **~5–25 ms off time-to-first-frame** (bigger % on cold
  boot), zero functional change (autostart apps still launch, just a few ms
  later — imperceptible to the user, they don't gate on the bar anyway).
- Effort: **S**. Risk: **Low** (pure reorder; autostart has no dependency
  on bar/panel state today — verified by reading `autostart.c`, it only
  touches config + `/proc`/exec, no `wl`/`egl`/`render` args).

### Wave 2 — medium effort, real but smaller wins (parallel with each other and with Wave 1; T2.1 touches `nvg.c`+`shape.c` only, disjoint from Wave 1's `main.c`/`audio.c`)

**T2.1 — lazy-load CJK/Devanagari/Emoji fallback fonts (implement docs/15's T2.1, now with startup-time justification)**
- Measured cost: CJK (33 ms) + Devanagari (10 ms) + Emoji (3 ms) = 46 ms,
  ~43% of `dc_render_ensure()`'s font-load time, spent cmap-parsing scripts
  this shell's configured user (English/Urdu, per memory
  `language-urdu-in-english-out`) is unlikely to ever render.
- Change: as docs/15 already specified — defer `add_fallback_font()` for
  CJK/Devanagari/Thai/Emoji until a coverage-miss on those codepoints is
  about to be shaped (hook in `render/shape.c`'s font-selection path); keep
  Latin (NotoSans) + Arabic eager (Urdu is the primary non-Latin case for
  this user). This was previously justified only by memory (~24 MB); now
  also justified by **~46 ms of avoidable first-frame latency**.
- Files: `src/render/nvg.c`, `src/render/shape.c` (unchanged file set from
  docs/15's original T2.1 spec).
- Expected win: **first-frame latency 108 ms → ~65 ms (cold) / ~35 ms →
  ~25 ms (warm)** for users who never trigger CJK/Devanagari/Emoji
  fallback (majority case for this shell's actual user).
- Effort: **M**. Risk: **Medium** (must not disturb an in-flight
  `nvgBeginFrame` when adding a fallback font mid-session — same caveat
  docs/15 already flagged; add between frames / guard).

**T2.2 — investigate the ~2 ms-apart first animation frames on panel open (Wave 3-adjacent, demote if no visual symptom)**
- Measured cost: not a CPU/latency problem per se (panel-open total latency
  is already only ~30 ms, §3) — flagged because 2 ms between swaps is far
  faster than a 16.6 ms vsync interval, meaning 1–2 frames' worth of GPU
  work may be thrown away by the compositor before the surface is even
  mapped/visible.
- Investigate: whether the very first 1–2 frame-callback `done` events
  after `wl_surface_commit()` on a brand-new surface fire eagerly (a
  well-known benign Wayland pattern for an as-yet-unmapped surface) or
  whether `bar.c`/`controlcenter.c`'s frame-callback re-arm logic is
  requesting a redraw it doesn't need to. Verify against the actual
  `configure`/`ack_configure` sequence timing.
- Files: none for investigation; `src/ui/controlcenter.c` /
  `src/ui/launcher.c` / `src/wayland/egl.c` if a guard turns out to be
  needed.
- Expected win: **uncertain, likely sub-frame** — primarily documents
  whether this is benign or a real waste.
- Effort: **S** (investigation only). Risk: **Low.**

### Wave 3 — low-ROI / reject-with-evidence (do not implement; recorded so nobody re-proposes them)

**T3.1 — `-O3`/LTO for startup hot paths — REJECT, measured**
- Full-tree `-O3` rebuild (incl. vendored nanovg/cJSON) showed **no
  measurable difference** vs `-O2` on either EGL init or font/cmap-parse
  time (both dlopen/I-O-cache-bound, not compute-bound) — see §9.
- Verdict: **do not implement.** Revisit only if a real CPU-bound hot loop
  is found via better profiling tooling later (none exists on this host
  today).

**T3.2 — restructure `bar_compute_signature()` to push-based dirty flags — DEFER, low ROI**
- Measured cost of the current full-recompute-every-tick approach is
  sub-millisecond (§7) — doesn't move the 0.15% idle-CPU number. A
  push-based rewrite (services signal "I changed" instead of the bar
  polling+hashing everything) would be a meaningful architectural change
  for negligible measured payoff.
- Verdict: **defer indefinitely** unless idle CPU regresses for an
  unrelated reason and this becomes the bottleneck.

**T3.3 — 1 Hz tick → adaptive/minute-granularity tick — DEFER, low ROI, real coupling risk**
- The 1000 ms tick interval is fixed regardless of `show_seconds`, but it
  also drives `dc_sysmon_poll()`/`dc_processes_refresh()`/
  `dc_dashboard_refresh()`/OSD-change detection, all of which have their
  own faster-than-a-minute cadences the bar's own clock granularity doesn't
  determine. Slowing it down only when `show_seconds` is false and no
  panel/OSD needs faster updates would need careful auditing of every
  tick-driven consumer, for a change that doesn't move the measured 0.15%
  CPU number at all.
- Verdict: **defer**; not worth the coupling-audit risk for an
  already-at-floor CPU number.

---

## Summary table (compact)

| Wave | Task | Measured cost | Expected win | Effort | Risk | Files |
|---|---|---|---|---|---|---|
| 1 | T1.1 parallelize EGL init w/ niri/dbus/autostart | 8–30 ms serialized unnecessarily | hides 8–30 ms of startup | S | Low | `main.c` |
| 1 | T1.2 async `wpctl` (no main-thread block) ⭐ | 35–40 ms UI freeze every ~10s + in startup path | −35-40 ms/10s idle stall, −35-40 ms startup | M | Low-Med | `audio.c` |
| 1 | T1.3 defer autostart past bar creation | 5–24.5 ms blocking bar creation | −5 to −25 ms startup | S | Low | `main.c` |
| 2 | T2.1 lazy CJK/Devanagari/Emoji fonts | 46 ms of 108 ms font-load, unused scripts | 108→~65 ms (cold) font load | M | Medium | `nvg.c`, `shape.c` |
| 2 | T2.2 investigate sub-vsync frame bursts on panel-open | unclear, likely benign | doc-only or sub-frame | S | Low | none / `controlcenter.c` |
| 3 | T3.1 `-O3`/LTO | measured: no effect | **REJECT** | — | — | — |
| 3 | T3.2 push-based damage signature | sub-ms today | **DEFER** | — | — | — |
| 3 | T3.3 adaptive tick interval | 0% CPU movement | **DEFER** | — | — | — |

**Combined Wave 1+2 target (cold boot): ~315 ms → ~230 ms time-to-first-frame
(~27% faster), plus elimination of the recurring ~35-40 ms/10s UI freeze
that exists today regardless of boot state.** All Wave 1 items are
SAFE-TO-IMPLEMENT-NOW (disjoint files: `main.c` for T1.1/T1.3, `audio.c` for
T1.2 — fully parallelizable). Wave 2's T2.1 needs care (mid-session font
add during an in-flight frame) but is functionally identical in scope to
docs/15's already-planned T2.1. Wave 3 is reject/defer, recorded to save a
future session from re-proposing already-measured dead ends.
