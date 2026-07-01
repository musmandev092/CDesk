# DankC — C Coding Conventions

Clean, professional, boring-on-purpose C. Optimise for readability and lifetime maintenance, not
cleverness. Every contributor (human or agent) follows this.

## Language & tooling
- **C11** (`-std=c11`), GCC/Clang. Warnings are errors in CI: `-Wall -Wextra -Wshadow -Wvla
  -Wpointer-arith -Wwrite-strings -Wno-unused-parameter`.
- Format with `clang-format` (`.clang-format` at repo root) before committing.
- 4-space indent, no tabs (except Makefile). 100-column soft limit.

## Naming
- **Types:** `snake_case` with a `_t` suffix for structs used by value/handle: `struct dc_bar`, typedef
  `dc_color_t`. Public types are prefixed **`dc_`** (DankC).
- **Functions:** `dc_<module>_<verb>()` — e.g. `dc_loop_add_fd()`, `dc_bar_render()`, `dc_log_init()`.
  Static/file-local functions may drop the `dc_` prefix but keep `<module>_` where helpful.
- **Variables:** descriptive `snake_case`. No single-letter names except loop indices (`i`, `j`) and
  well-understood locals (`fd`, `n`). No Hungarian notation.
- **Constants / macros:** `UPPER_SNAKE_CASE`, prefixed `DC_` when public (`DC_BAR_HEIGHT_DEFAULT`).
- **Enums:** `dc_<thing>` type, members `DC_<THING>_<VALUE>`.
- Booleans read as questions: `bar->is_hidden`, `output_is_ready(o)`.

## Structure
- One module = one `.c` + one `.h` under `src/<area>/`. The header exposes the minimal public surface;
  everything else is `static`.
- Each header has an include guard `#ifndef DC_<AREA>_<NAME>_H`. Include order in `.c`: matching header
  first, then system, then project headers.
- No god-objects. State lives in a struct owned by its module; pass it explicitly (no hidden globals
  except a single logging sink and the interned Wayland globals, both clearly marked).

## Memory & errors
- **Ownership is explicit and documented.** For every `dc_x_create()` there is a `dc_x_destroy()`.
  The comment on a function that returns a pointer states who frees it.
- Check every allocation; on failure log and unwind cleanly (goto-based cleanup ladders are fine and
  encouraged for multi-step init).
- Functions that can fail return `int` (`0` ok, negative errno-style) or a pointer (`NULL` on failure);
  never both meanings in one channel. Out-params via pointers.
- No `malloc` in the hot render path — use arenas / preallocated buffers (`render/`).
- Zero-initialise structs (`= {0}`), free-then-NULL to avoid double free.

## Wayland / EGL / GL specifics
- Wrap every protocol object in a small owning struct; listeners get that struct as `user_data`.
- Never block the event loop on I/O; D-Bus calls are async, heavy work is deferred.
- Re-arm frame callbacks **only while animating** (idle = 0% CPU). Document any code that pins redraws.
- GL state changes are localised and restored; the render code says which invariants it assumes.

## Comments
- Comment **why**, not **what**. A function's header comment states its contract (ownership, blocking,
  thread/loop expectations), not a restatement of its body.
- Mark protocol-spec-mandated ordering (e.g. "must ack_configure before attaching a buffer") inline.
- `TODO(scope):` and `FIXME(scope):` with a short reason; no bare TODOs.

## Commits & git
- Small, logical commits. Conventional-commit prefixes: `feat`, `fix`, `build`, `chore`, `docs`,
  `refactor`, `perf`, `test`. Scope in parens: `feat(bar): render clock widget`.
- Each commit builds. Never commit generated protocol code (`.gitignore`d) or build output.
- The design docs in `docs/` are the source of truth; when behaviour changes, update the doc in the
  same commit.

## Definition of done (per feature)
1. Builds clean with warnings-as-errors.
2. Runs on the reference niri session without leaking (checked with a run + teardown).
3. Matches the spec in `docs/` (look/flow/perf); deviations are documented.
4. RAM/CPU sanity-checked against the "lightweight" goal.
