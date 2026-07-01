#!/usr/bin/env bash
# DankC dev helper — build, run, screenshot, measure. See AGENTS.md.
set -euo pipefail
cd "$(dirname "$0")/.."

log=/tmp/dankc.log

case "${1:-}" in
build)
    make
    ;;
run)
    make && exec ./bin/dankc
    ;;
bg)
    make
    pkill -x dankc 2>/dev/null || true
    nohup ./bin/dankc >"$log" 2>&1 &
    echo "dankc running (pid $!), logging to $log"
    ;;
stop)
    pkill -x dankc && echo "stopped" || echo "not running"
    ;;
shot)
    dir="${2:-/tmp}"
    dms screenshot all -d "$dir" --filename dankc.png --no-clipboard --no-notify
    echo "$dir/dankc.png"
    ;;
rss)
    pid="$(pgrep -x dankc | head -1)"
    [ -n "$pid" ] || { echo "dankc not running"; exit 1; }
    awk '/^Pss:/{p+=$2}/^Private/{r+=$2} END{printf "dankc Pss=%dMB Private=%dMB\n", p/1024, r/1024}' \
        "/proc/$pid/smaps_rollup"
    ;;
log)
    tail -n "${2:-40}" "$log"
    ;;
*)
    echo "usage: scripts/dev.sh {build|run|bg|stop|shot [dir]|rss|log [n]}"
    exit 1
    ;;
esac
