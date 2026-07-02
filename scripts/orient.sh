#!/bin/sh
# orient.sh — one-shot orientation for a fresh session. Prints project state,
# build status, code map, and where to read next. Run: sh scripts/orient.sh
set -e
cd "$(dirname "$0")/.."

echo "=================================================================="
echo " DankC — project orientation ($(date +%Y-%m-%d))"
echo "=================================================================="
echo
echo "## READ THESE FIRST (in order):"
echo "  AGENTS.md              — canonical current state (no re-scan needed)"
echo "  docs/TASKS.md          — backlog: what's done / open"
echo "  docs/POLISH.md         — UI/UX + perf gaps vs DMS (the improvement list)"
echo "  docs/10-DESIGN-SYSTEM.md, 11-UX-FLOW.md — exact DMS tokens/shaders/motion"
echo "  CONVENTIONS.md         — coding standards"
echo "  ~/.claude/.../memory/  — persistent gotchas (deadlock, listener stubs, ...)"
echo
echo "## TASK STATUS"
printf "  done: %s   open: %s\n" \
  "$(grep -cE '^\- \[x\]' docs/TASKS.md 2>/dev/null)" \
  "$(grep -cE '^\- \[ \]' docs/TASKS.md 2>/dev/null)"
grep -nE '^\- \[ \]' docs/TASKS.md 2>/dev/null | sed 's/^/  OPEN /' || true
echo
echo "## GIT"
printf "  commits: %s   branch: %s   dirty: %s\n" \
  "$(git rev-list --count HEAD 2>/dev/null)" \
  "$(git branch --show-current 2>/dev/null)" \
  "$([ -n "$(git status --porcelain 2>/dev/null)" ] && echo yes || echo no)"
git log --oneline -8 2>/dev/null | sed 's/^/    /' || true
echo
echo "## CODE MAP  (first-party LOC)"
printf "  C sources: %s   C++ : %s   headers: %s\n" \
  "$(find src -name '*.c' | wc -l)" "$(find src -name '*.cpp' | wc -l)" "$(find src -name '*.h' | wc -l)"
echo "  by area:"
for d in core wayland render ui ui/bar services niri theme ipc; do
  loc=$(cat src/$d/*.c src/$d/*.cpp 2>/dev/null | wc -l)
  [ "$loc" -gt 0 ] && printf "    %-14s %6s LOC\n" "$d" "$loc"
done
echo "  total: $(cat $(find src -name '*.c' -o -name '*.cpp') | wc -l) LOC"
echo
echo "## BUILD  (both must be green)"
echo "  make            -> ./bin/dankc          (fallback, fastest)"
echo "  meson setup build && meson compile -C build   (primary)"
echo
echo "## RUN & DRIVE"
echo "  ./bin/dankc &                              # start the shell"
echo "  ./bin/dankc ctl <cmd>                      # launcher|control-center|"
echo "      notifications|clipboard|settings|lock|screenshot|night|"
echo "      color-picker|screenshot-region|quit"
echo "  ./bin/dankc keybinds                       # niri keybind snippet"
echo "  DANKC_LOCK_ESCAPE=1 ./bin/dankc            # F1 + 'ctl unlock' escape (test lock safely)"
echo "  Demo opens:  DANKC_CC_DEMO=1 / DANKC_OSD_DEMO=1 / DANKC_LAUNCHER_DEMO=1 / DANKC_NC_DEMO=1"
echo
echo "## VERIFY UI (grim available)"
echo "  * Stop the user's DMS first so two top-bars don't fight the layer, then:"
echo "  grim shot.png ; grim -o <output> shot.png    # 'niri msg outputs' for names"
echo "  Open each panel via 'dankc ctl ...' and screenshot; compare to DMS QML/live."
echo
echo "## DMS REFERENCE"
echo "  ~/Downloads/DankMaterialShell-master/quickshell/   (QML source to match)"
echo "  https://danklinux.com/docs/                          (docs)"
echo "=================================================================="
