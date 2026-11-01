#!/usr/bin/env bash
# Owned Linux display capture only; runtime acceptance is a separate result.
set -euo pipefail
[[ ${1:-} == O4 && $# == 1 ]] || { echo 'usage: record.sh O4' >&2; exit 1; }
root=$(cd -- "$(dirname -- "$0")/../.." && pwd)
evidence=/workspace/x64-run/v2/R5/O4
video=/workspace/x64-run/videos/R5-O4.mp4
mkdir -p "$evidence"
exec > >(tee /workspace/x64-run/v2/R5/O4-recorder.log) 2>&1
xvfb_pid= capture_pid= ui_pid=
cleanup() {
    recorder_rc=$?
    echo "$recorder_rc" > /workspace/x64-run/v2/R5/O4-recorder.exit
    if [[ $recorder_rc != 0 ]]; then
        echo "BLOCKED: capture/scenario failed (exit $recorder_rc); see O4 evidence" > /workspace/x64-run/v2/R5/O4-capture.status
    fi
    # Only processes started by this invocation; no display-wide cleanup.
    if [[ -n $ui_pid ]]; then kill "$ui_pid" 2>/dev/null || true; wait "$ui_pid" 2>/dev/null || true; fi
    if [[ -n $capture_pid ]]; then
        printf 'q\n' >&8 || true
        wait "$capture_pid" 2>/dev/null || true
    fi
    if [[ -n $xvfb_pid ]]; then kill "$xvfb_pid" 2>/dev/null || true; wait "$xvfb_pid" 2>/dev/null || true; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM
# Lock/socket checks also reject stale reservations; never reuse another display.
if [[ -e /tmp/.X99-lock || -S /tmp/.X11-unix/X99 ]] || DISPLAY=:99 xdpyinfo >/dev/null 2>&1; then
    echo 'BLOCKED: display :99 occupied or reserved' | tee /workspace/x64-run/v2/R5/O4-capture.status
    exit 1
fi
# Preserve previous evidence rather than mixing runs.
if [[ -e $evidence/scenario.json ]]; then
    mv "$evidence" "$evidence-previous-$(date +%s%N)"
    mkdir -p "$evidence"
fi
Xvfb :99 -screen 0 1280x720x24 -nolisten tcp > "$evidence/xvfb.log" 2>&1 &
xvfb_pid=$!
export DISPLAY=:99
for ((i=0; i<50; i++)); do
    kill -0 "$xvfb_pid" || exit 1
    if xdpyinfo >/dev/null 2>&1; then break; fi
    sleep .1
done
xdpyinfo >/dev/null
mkfifo "$evidence/ffmpeg.stdin"
exec 8<>"$evidence/ffmpeg.stdin"
ffmpeg -y -f x11grab -video_size 1280x720 -framerate 15 -i :99.0 \
    -t 58 -c:v libx264 -preset ultrafast -crf 20 -pix_fmt yuv420p \
    "$video" <&8 > "$evidence/ffmpeg.log" 2>&1 &
capture_pid=$!
cd "$root"
# Timeout bounds the visible app; each adapter worker separately owns and bounds
# its child session (20 seconds plus cleanup), including on runtime failure.
timeout --kill-after=5s 54s xterm -geometry 142x36+0+0 -fa Monospace -fs 11 \
    -bg '#101820' -fg '#eeeeee' -title 'Linux prototype — Xbox validation pending' \
    -e bash -c 'python3 x64/ui/app.py --demo-evidence "$1" 2>"$1/app.stderr"; rc=$?; echo "$rc" >"$1/ui.exit"; exit "$rc"' _ "$evidence" \
    > "$evidence/xterm.log" 2>&1 &
ui_pid=$!
# Do not redirect curses stdout: its rendering must go to the xterm PTY.
rc=0
wait "$ui_pid" || rc=$?
ui_pid=
echo "$rc" > "$evidence/xterm.exit"
printf 'q\n' >&8 || true
capture_rc=0
wait "$capture_pid" || capture_rc=$?
capture_pid=
echo "$capture_rc" > "$evidence/capture.exit"
[[ $rc == 0 && $capture_rc == 0 ]]
python3 - "$evidence" <<'PY'
import json, pathlib, sys
p = pathlib.Path(sys.argv[1])
s = json.loads((p / 'scenario.json').read_text())
assert s['completed'] and s['launch_status'] == 'blocked' and s['launch_exit'] == 2
assert (p / 'ui.exit').read_text().strip() == '0'
assert (p / 'launch.exit').read_text().strip() == '2'
assert 'LAUNCH BLOCKED' in (p / 'ui.log').read_text()
assert '[launcher] exec' in (p / 'launch.log').read_text()
PY
ffprobe -v error -show_entries stream=codec_name -show_entries format=duration -of json "$video" > "$evidence/ffprobe.json"
python3 - "$evidence/ffprobe.json" <<'PY'
import json, sys
s = json.load(open(sys.argv[1]))
assert s['streams'][0]['codec_name'] == 'h264'
assert 0 < float(s['format']['duration']) <= 60
PY
# Final five-second hold is rendered before the app exits; extract within it.
ffmpeg -y -sseof -3 -i "$video" -frames:v 1 "$evidence/final.png" > "$evidence/screenshot.log" 2>&1
cp "$evidence/final.png" /workspace/x64-run/v2/R5/O4-final.png
echo 'CAPTURE PASS; launch.exit=2 / LAUNCH BLOCKED; Linux prototype — Xbox validation pending' | tee /workspace/x64-run/v2/R5/O4-capture.status
