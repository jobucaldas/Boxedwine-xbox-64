#!/usr/bin/env bash
# Linux-box evidence only. Starts a private Xvfb; never uses someone else's display.
set -euo pipefail
order=${1:?usage: record.sh O1 -- command args...}
shift
[[ $order =~ ^O[1-7]$ && ${1:-} == -- ]] || exit 2
shift
(($#)) || exit 2
for tool in Xvfb xterm ffmpeg ffprobe xdpyinfo setsid; do command -v "$tool" >/dev/null || { echo "missing capture tool: $tool" >&2; exit 127; }; done
base=/workspace/x64-run
mkdir -p "$base/videos" "$base/v2/R4H"
work=$(mktemp -d)
xpid= tpid= fpid=
cleanup() {
    if [[ -n $fpid ]]; then kill -INT "$fpid" 2>/dev/null || true; wait "$fpid" 2>/dev/null || true; fi
    if [[ -n $tpid ]]; then kill "$tpid" 2>/dev/null || true; wait "$tpid" 2>/dev/null || true; fi
    if [[ -n $xpid ]]; then kill "$xpid" 2>/dev/null || true; wait "$xpid" 2>/dev/null || true; fi
    rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$work/display" >"$work/xvfb.log" 2>&1 &
xpid=$!
for ((i=0;i<100;i++)); do [[ -s $work/display ]] && break; sleep .05; done
export DISPLAY=:$(cat "$work/display")
xdpyinfo >/dev/null
cat > "$work/run" <<'RUN'
#!/usr/bin/env bash
work=$1
log=$2
shift 2
exec > >(tee "$log") 2>&1
printf 'R4H terminal proof on Linux box (not Xbox device)\n'
if [[ -n ${UWPH_RECORD_LABEL:-} ]]; then printf 'Recording label: %s\n' "$UWPH_RECORD_LABEL"; fi
printf 'Command:'
printf ' %q' "$@"
printf '\n'
setsid timeout --kill-after=2s 50s "$@" &
cpid=$!
trap 'kill -- -"$cpid" 2>/dev/null || true' EXIT
trap 'exit 130' INT
trap 'exit 143' TERM HUP
wait "$cpid"
rc=$?
trap - EXIT INT TERM HUP
printf '\nRecorded command exit status: %d\n' "$rc"
printf '%d\n' "$rc" > "$work/status.tmp"
mv "$work/status.tmp" "$work/status"
# Keep the result readable while the recorder closes.
sleep 8
RUN
chmod +x "$work/run"
video=$base/videos/R4H-$order.mp4
ffmpeg -hide_banner -loglevel warning -y -f x11grab -framerate 15 -video_size 1280x720 -i "$DISPLAY" -t 58 -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$video" >"$base/v2/R4H/$order-ffmpeg.log" 2>&1 &
fpid=$!
sleep .5
xterm -geometry 126x34+0+0 -fa Monospace -fs 12 -bg black -fg white -e "$work/run" "$work" "$base/v2/R4H/$order-record.log" "$@" &
tpid=$!
for ((i=0;i<550;i++)); do [[ -f $work/status ]] && break; sleep .1; done
if [[ -f $work/status ]]; then rc=$(cat "$work/status"); else rc=124; fi
sleep 3
kill -INT "$fpid" 2>/dev/null || true
encoder_rc=0
wait "$fpid" || encoder_rc=$?
# ffmpeg returns 255 after the requested SIGINT, after finalizing the MP4.
if ((encoder_rc != 0 && encoder_rc != 255)); then exit "$encoder_rc"; fi
fpid=
printf '%d\n' "$rc" > "$base/v2/R4H/$order-record.exit"
ffprobe -v error -select_streams v:0 -show_entries stream=codec_name -show_entries format=duration -of default=noprint_wrappers=1 "$video"
echo "recorded command exit status: $rc"
exit "$rc"
