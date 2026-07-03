# DankC — Performance & Footprint Optimization Plan (P8)

Definitive, measurement-backed optimization backlog. **Analysis only** — this
doc plans work; other agents implement. Builds on the earlier partial pass
(docs/POLISH.md §P7: damage tracking, poll reduction, Material Symbols font
subset — all DONE, not re-listed here).

Baseline captured 2026-07-03 on the live niri session (Intel/Mesa `iris`,
hardware GPU node `/dev/dri/renderD128` confirmed open — **not** llvmpipe
software rendering), single output ("Winit", 2560×44 bar), own PID, `-O2 -g`
build.

---

## (a) Measured baseline

| Metric | Idle bar (no panels) | With a panel open |
|---|---|---|
| VmRSS | **175.8 MB** (175640 kB, flat over 10×2 s samples) | 181.7 MB (launcher) / 175.9 MB (control center) / 182.0 MB (settings) |
| Pss (smaps_rollup) | **71.0 MB** | 76.9 MB (launcher) / 71.1 MB (CC) / 77.2 MB (settings) |
| Private (Dirty+Clean) | **41.3 MB** (Private_Dirty 40.4 MB, Private_Clean 0.9 MB) | 47.1 MB (launcher) / 47.4 MB (settings) |
| Shared_Clean | 134.4 MB (Mesa/libc/etc. mapped from disk, shared with niri & other GL clients) | — |
| Idle CPU (utime+stime Δ / CLK_TCK=100) | **0.08 – 0.20 %** over 10–12 s windows (1–2 ticks; at the measurement noise floor) | clock ticking at 1 Hz does not raise it measurably |
| Idle GPU redraws (`DANKC_RENDER_STATS=1`) | 60 tick calls/60 s → **2–16 GL frames drawn, 44–59 skipped**; trends to ~2/60 at true idle (the residual is the cpu/mem widget reacting to dev-machine load, not idle noise) | — |
| Threads | 5 (main + Mesa/gallium util: `disk$0`, `sh0`, `traceq0`, `gdrv0`) | — |
| Open fds | 15 (4× `/dev/dri/renderD128`, 5 sockets, 1 timerfd, log, pidfd) | — |
| Binary | 4.03 MB on disk (`text 908 KB / data 23 KB / bss 338 KB`), **not stripped** (debug_info); only 0.86 MB Pss resident (text pages) | — |

Reference (read-only, same session): DMS `qs` measured **Pss 595 MB / Private
521 MB / VmRSS 803 MB**. DankC is already ~8× smaller by Pss.

Panel open/close is **lazy and clean**: a panel's EGL window surface is created
in its `*_render()` on first show and destroyed in `*_teardown()` on close
(e.g. `src/ui/controlcenter.c:1543` create / `:2004` `cc_teardown` finish). RSS
returns to baseline after close (175.9 MB after CC close). So the idle baseline
is genuinely bar-only; hidden panels cost ~0.

**Not measured / unmeasured:** per-syscall wakeup profile (no `strace`/`bpftrace`
on host; `perf trace` blocked by `perf_event_paranoid=2` + no BPF). Fork cadence
below is derived from source cache windows, not a live syscall trace. Two-output
RSS not re-measured (nested niri exposed one output) — but see the shared-context
finding in (c) Wave 3.

---

## (b) Memory attribution

Aggregated from `/proc/<pid>/smaps` at idle (Pss = per-process share; Rss =
total resident incl. shared):

| Region | Pss | Rss | Nature | Attributable to |
|---|---:|---:|---|---|
| `[heap]` | **31.9 MB** | 31.9 MB | 100 % Private_Dirty | **font data + app state (see below)** |
| `libLLVM.so.22.1` | 22.8 MB | 85.5 MB | Shared_Clean 83 MB | Mesa gallium shader compiler (NIR→LLVM), pulled in by `libgallium` |
| `libgallium-26.1.3.so` | 9.5 MB | 40.0 MB | Shared_Clean 38.5 MB | Mesa `iris` driver |
| `[anon]` (39 regions) | 3.7 MB | 3.7 MB | Private | GL/EGL scratch, nanovg VBOs, back buffers |
| dankc binary (text) | 0.86 MB | — | mostly Private_Clean | our code |
| `libicudata` | 0.26 MB | 2.1 MB | Shared_Clean | HarfBuzz/ICU tables |
| everything else (libstdc++, EGL_mesa, icuuc, systemd, freetype, fontconfig, …) | ~1.9 MB total | — | Shared_Clean | libs |
| **Total** | **~71 MB** | ~175 MB | | |

**Three regions = 90 % of Pss:** heap (32) + libLLVM (23) + libgallium (9.5) =
64 MB. RSS is dominated by Mesa: libLLVM 85 MB + gallium 40 MB = **125 MB of the
175 MB RSS is the Mesa driver stack**, but it is Shared_Clean (mapped from the
`.so` on disk, shared with niri and every other GL client) so its true
*incremental* cost is the ~32 MB Pss, not 125 MB.

### What is actually in the 32 MB heap — the key finding

fontstash loads every font by `malloc(fileSize) + fread` of the **whole file**
(`third_party/nanovg/fontstash.h:955-961`, via `nvgCreateFont` →
`fonsAddFont`). We load 8 faces (`src/render/nvg.c` — 1 UI, 1 icon subset, 6
fallbacks). File sizes:

| Font | On-disk size → heap | Purpose |
|---|---:|---|
| **NotoSansCJK-Regular.ttc** | **19.5 MB** | CJK fallback (zh/ja/ko) — **never shown** for an English/Urdu user |
| NotoEmoji-Regular.ttf | 2.0 MB | monochrome emoji fallback |
| InterVariable.ttf | 0.88 MB | UI font |
| NotoSans-Regular.ttf | 0.62 MB | Cyrillic/Greek fallback |
| MaterialSymbolsRounded.subset.ttf | 0.30 MB | icons (already subset in P7) |
| NotoSansArabic-Regular.ttf | 0.23 MB | Urdu/Arabic fallback |
| NotoSansDevanagari-Regular.ttf | 0.24 MB | Hindi fallback |
| NotoSansThai-Regular.ttf | 0.04 MB | Thai fallback |
| **Σ font data** | **~23.8 MB** | all resident as Private_Dirty heap |

So **~24 of the 32 MB heap is font file bytes fread into private-dirty RAM**,
and **19.5 MB of that is one CJK fallback** the target user never renders. The
remaining ~8 MB heap is nanovg's glyph atlas (default 512×512 R8 ≈ 256 KB — not
the problem), the cmap coverage bitmaps, cJSON config, the app index, and
service caches.

(Split is inferred from source + the 32 MB heap total, not from a per-`malloc`
trace; the 24 MB font figure is exact file-size arithmetic.)

**Not the problem (verified):** one shared `NVGcontext` (single glyph atlas —
`nvgCreateGLES3` called once, `src/render/nvg.c:466`); one shared `EGLContext`
(single `eglCreateContext`, `src/wayland/egl.c:51`, share list = every panel &
bar surface). No per-panel or per-monitor atlas/context duplication. The
Material Symbols variable font was already subset 14.5 MB → 301 KB in P7.

---

## (c) Ranked optimization backlog (waves)

Waves group tasks that **don't touch the same files** so they can be dispatched
in parallel. Within the doc, higher impact-per-risk first. Effort S/M/L.

### Wave 1 — high impact, low risk (fully parallel; disjoint files)

**T1.1 — mmap font files instead of `fread`-into-heap** ⭐ biggest win
- Measured cost: ~24 MB Private_Dirty heap holds full font-file bytes; 19.5 MB
  is the CJK `.ttc` alone.
- Change: replace `nvgCreateFont(path)` with `mmap(PROT_READ, MAP_PRIVATE)` of
  the file + `nvgCreateFontMem`/`nvgCreateFontMemAtIndex(vg, name, ptr, len,
  /*freeData=*/0, index)` (both already exist —
  `third_party/nanovg/nanovg.h:567,570`). Keep the mmap alive for process
  lifetime (store ptr+len per face; munmap on shutdown). CJK `.ttc` uses index 0
  (matches current behaviour). This moves all font bytes from Private_Dirty heap
  to **file-backed Shared_Clean pages** — only the glyph-lookup pages actually
  touched fault in (a few hundred KB), and they are evictable.
- Files: `src/render/nvg.c` (`load_font`, `add_fallback_font`,
  `load_fallback_fonts`, the ICON/UI/EMOJI candidate loaders). No nanovg/
  fontstash edits needed (public `*Mem*` API already handles freeData=0).
- Expected saving: **Pss −18 to −22 MB (71 → ~50 MB); Private_Dirty −20 MB;
  RSS −18 to −22 MB.** Heap 32 → ~10 MB.
- Effort: **M.** Risk: **Low** (must not munmap while a face is live — lifetime
  is process-long; cmap parser already reads files independently).

**T1.2 — glibc malloc arena tuning + post-init trim**
- Measured cost: 5 threads (Mesa gallium util threads) → glibc default arena max
  = 8×nproc; multi-arena fragmentation inflates the non-font ~8 MB heap and the
  3.7 MB `[anon]`.
- Change: `mallopt(M_ARENA_MAX, 2)` (or `MALLOC_ARENA_MAX=2` in the packaged
  launcher/env) before the first threaded GL call, and one `malloc_trim(0)`
  after the startup allocation burst (config load, app index, font/cmap parse)
  settles.
- Files: `src/main.c` (startup, after init, before `dc_loop_run`).
- Expected saving: **RSS/Pss −2 to −5 MB** (frees startup-transient heap;
  reduces arena reservation).
- Effort: **S.** Risk: **Low** (standard glibc knobs; verify no latency
  regression from arena contention — only 5 threads, negligible).

**T1.3 — strip installed binary + confirm full Material Symbols font not shipped**
- Measured cost: 4.03 MB binary is mostly `debug_info` (only 0.86 MB text is
  resident — **no RAM win**, disk/footprint only). Also confirm the 14.5 MB full
  `MaterialSymbolsRounded.ttf` is *not* installed (P7 says only the subset is).
- Change: `strip`/`install_mode` in `meson.build` install step + packaging;
  assert install manifest excludes the full font (keep it in-repo as the subset
  regeneration source only).
- Files: `meson.build`, `packaging/`.
- Expected saving: on-disk −3 MB binary, install footprint sanity. **0 MB RAM.**
- Effort: **S.** Risk: **Low** (keep unstripped in dev build; strip only on
  install).

### Wave 2 — medium impact/effort (T2.1 sequences after T1.1; T2.2/T2.3 parallel)

**T2.1 — lazy fallback-font loading** (do AFTER T1.1 — same file `nvg.c`)
- Measured cost: all 6 fallback faces are loaded + cmap-parsed at startup
  regardless of whether their script is ever displayed. Post-T1.1 the byte cost
  is neutralized (mmap), but startup still `mmap`s + cmap-parses 6 files and
  faults their table-directory pages.
- Change: defer `add_fallback_font()` for CJK/Devanagari/Thai/Emoji until the
  first codepoint in that script is about to be shaped
  (`dc_render_font_for_codepoint` / `render/shape.c` already resolves per-font
  coverage — hook the load there on a coverage miss). Keep Latin (NotoSans) +
  Arabic (Urdu is the primary non-Latin case) eager.
- Files: `src/render/nvg.c`, `src/render/shape.c`.
- Expected saving: **faster startup + a few hundred KB fewer faulted pages**;
  the CJK `.ttc` table directory never faults for an English/Urdu user. Marginal
  on top of T1.1 but removes the last CJK residency.
- Effort: **M.** Risk: **Medium** (adding a fallback font mid-frame must not
  disturb an in-flight `nvgBeginFrame`; add between frames / guard).

**T2.2 — replace `nmcli` SSID poll with NetworkManager D-Bus subscription**
- Measured cost (derived from source, unmeasured live): `dc_net_wifi()` is
  called every ~1 Hz bar tick and forks `nmcli dev wifi list` on a **3 s** cache
  window (`src/services/net.c:20,46`) → ~20 `fork`+`exec` per minute at idle.
  Forking a 175 MB-RSS process copies page tables (COW) — real churn even though
  aggregate CPU reads 0.08 %.
- Change: subscribe to `org.freedesktop.NetworkManager`
  `PropertiesChanged`/`ActiveConnection`/`AccessPoint.Strength` on the **already-
  connected system sd-bus** (`src/services/dbus.c`); cache SSID+signal, refresh
  only on signal. Zero forks at idle.
- Files: `src/services/net.c` (+ reuse `src/services/dbus.c` bus).
- Expected saving: **−~20 forks/min**, eliminates periodic COW spikes; small
  idle-CPU + battery win. **~0 MB RSS.**
- Effort: **M.** Risk: **Medium** (NM object-path plumbing; keep the sysfs
  link-up path as fallback when NM is absent).

**T2.3 — widen `wpctl` volume cache 3 s → 8–10 s** (parallel with T2.2)
- Measured cost: `wpctl get-volume` forks on a **3 s** cache miss
  (`src/services/audio.c:21`) → ~20 forks/min at idle. Native libpipewire
  subscription is explicitly out of scope this pass (per P7).
- Change: widen `DC_AUDIO_CACHE_SECONDS` to 8–10 s. Own slider/OSD writes
  already invalidate the cache immediately (`audio.c:84`), so only *external*
  volume changes wait longer — acceptable for a bar readout.
- Files: `src/services/audio.c`.
- Expected saving: **~13 fewer forks/min.** ~0 MB RSS.
- Effort: **S.** Risk: **Low** (only trade-off: external volume change shows up
  to ~10 s late on the bar; OSD still instant).

### Wave 3 — investigation / low-ROI (schedule last; may yield "no change")

**T3.1 — Mesa/LLVM footprint investigation**
- Measured cost: libLLVM 22.8 Pss / 85 Rss + gallium 9.5 Pss / 40 Rss = the RSS
  elephant, but Shared_Clean (incremental cost ≈ 32 MB Pss, shared with niri).
- Investigate: whether `iris` can run without the LLVM-linked gallium path
  (unlikely — LLVM is the shader backend); whether `MESA_GLThread=false` / fewer
  gallium worker threads drops a util thread + its arena; whether an EGL config
  with no stencil-elsewhere trims anon buffers. **Verify each against smaps —
  assume nothing.** Likely conclusion: unavoidable while on Mesa GLES; document
  the floor.
- Files: none (env/config experiments) or `src/wayland/egl.c` if a config knob
  helps.
- Expected saving: **uncertain, likely 0–5 MB**; primarily produces a documented
  RSS floor.
- Effort: **L.** Risk: **Low** (measurement only) — but low expected payoff, so
  last.

**T3.2 — two-output regression guard**
- Confirmed by code read that both bars share one `EGLContext` + one
  `NVGcontext`/atlas (only a `wl_egl_window` + EGLSurface per output ≈ 450 KB
  framebuffer each) — so 2 monitors is **not** 2× Mesa. Add a lightweight
  startup assertion/log that exactly one context + one atlas exist, to prevent a
  future refactor from regressing into per-surface contexts.
- Files: `src/render/nvg.c`, `src/wayland/egl.c`.
- Expected saving: preventive (protects the existing shared-context win).
- Effort: **S.** Risk: **Low.**

---

## (d) Realistic target

Pss is the meaningful per-process cost (RSS double-counts Mesa `.so`s shared
with niri). Concentrating on Wave 1 + Wave 2:

| | Baseline | After W1 (T1.1+T1.2) | After W1+W2 | Hard floor |
|---|---:|---:|---:|---:|
| **Pss** | 71 MB | **~48–50 MB** | ~47–49 MB | ~45 MB (Mesa 32 Pss + our irreducible ~13) |
| **VmRSS** | 176 MB | **~152–155 MB** | ~152 MB | ~150 MB (Mesa 125 Rss + heap ~10 + libs) |
| **Private_Dirty** | 40 MB | **~18–20 MB** | ~18 MB | — |
| **Idle CPU** | 0.08–0.2 % | unchanged (already floor) | ~same, **~40 fewer forks/min** | <0.5 % |
| **Idle GPU** | ~2 draws/60 s | unchanged (P7 damage tracking already near-zero) | — | — |

**Achievable: Pss ≈ 48 MB (−32 %), RSS ≈ 152 MB (−13 %), idle CPU < 0.3 % with
subprocess-fork churn cut ~90 %.** The single dominant lever is **T1.1 (mmap
fonts)** — it alone accounts for ~20 of the ~23 MB Pss reduction. RSS below
~150 MB is not reachable without leaving Mesa GLES (out of scope). Idle CPU and
idle GPU are already effectively optimal from P7; Wave 2 improves them at the
margin (fewer forks) rather than in the aggregate percentage.
