#!/usr/bin/env bash
# Run LibertyRecomp through weston, per project policy.
#
# weston is nested in the existing X11 display so its Xwayland keeps DRI3 and
# RADV can present. The headless backend does not: Mesa then reports "No DRI3
# support detected - required for presentation" and the run dies at startup.
#
# weston is long-lived: it starts once and is left running. Only the game is
# killed, by the caller. Reuse the same socket on every run.
#
# Usage: tools/run_weston.sh [LibertyRecomp args...]
#   BIN            binary to run        (default: build-linux/.../LibertyRecomp)
#   OUTER_DISPLAY  X display for weston (default: :1)
#   WESTON_SOCKET  weston socket name   (default: wayland-recomp)
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
bin="${BIN:-$root/build-linux/LibertyRecomp/LibertyRecomp}"
outer_display="${OUTER_DISPLAY:-:1}"
socket="${WESTON_SOCKET:-wayland-recomp}"
export XDG_RUNTIME_DIR="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
state_dir="${TMPDIR:-/tmp}/opencode"
log="$state_dir/weston-$socket.log"
pidfile="$state_dir/weston-$socket.pid"
mkdir -p "$state_dir"

if [ ! -x "$bin" ]; then
    echo "run_weston: no executable at $bin" >&2
    exit 1
fi

weston_pid=""
if [ -f "$pidfile" ]; then
    weston_pid="$(cat "$pidfile")"
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        echo "run_weston: stale weston pid $weston_pid, restarting" >&2
        weston_pid=""
    fi
fi

if [ -z "$weston_pid" ]; then
    rm -f "$XDG_RUNTIME_DIR/$socket" "$XDG_RUNTIME_DIR/$socket.lock"
    : > "$log"
    # setsid detaches weston from this command's process group so it survives
    # the caller; the game is the only process a run should kill.
    DISPLAY="$outer_display" setsid weston --backend=x11-backend.so --socket="$socket" \
        --xwayland --width=1280 --height=720 --log="$log" >/dev/null 2>&1 &
    weston_pid=$!
    echo "$weston_pid" > "$pidfile"
    echo "run_weston: started weston pid=$weston_pid" >&2
fi

inner_display=""
for _ in $(seq 1 100); do
    inner_display="$(sed -n 's/.*xserver listening on display \(:[0-9]*\).*/\1/p' "$log" 2>/dev/null | head -1)"
    [ -n "$inner_display" ] && break
    if ! kill -0 "$weston_pid" 2>/dev/null; then
        echo "run_weston: weston exited during startup; see $log" >&2
        rm -f "$pidfile"
        exit 1
    fi
    sleep 0.1
done

if [ -z "$inner_display" ]; then
    echo "run_weston: weston never started Xwayland; see $log" >&2
    exit 1
fi

echo "run_weston: weston pid=$weston_pid outer DISPLAY=$outer_display inner DISPLAY=$inner_display" >&2
DISPLAY="$inner_display" SDL_VIDEODRIVER=x11 "$bin" "$@"
