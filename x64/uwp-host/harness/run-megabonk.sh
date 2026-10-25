#!/usr/bin/env bash
# Linux THROUGH-uwph evidence, using the same loader/shim path as run-hello.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
harness=$PWD
work=$harness/out/megabonk
video=/workspace/x64-run/videos/B11-O1.mp4
recording=/workspace/x64-run/videos/B11-O1.recording.mp4
game=/workspace/steam-owned/games/megabonk
mkdir -p "$work" "$(dirname "$video")"
exec 9>"$work/lock"
flock -n 9 || { echo "run-megabonk: $work/lock held by another run" >&2; exit 1; }
make out/libuwph_wine_shim.so out/run_via_wine out/wine-wx/.stamp
[[ -f $game/Megabonk.exe && -f $game/GameAssembly.dll ]]
if [[ ! -d $work/prefix ]]; then
    cp -a --reflink=auto /workspace/megabonk/pfx-stock "$work/prefix"
fi
export WINEPREFIX=$work/prefix WINEDLLOVERRIDES=wineboot.exe=d
export WINEDEBUG=-all,+loaddll WINE_LOADER=$harness/out/wine-wx/lib/wine/wine64
xpid= fpid= server_pid=
cleanup() {
    if [[ -n $fpid ]]; then kill -INT "$fpid" 2>/dev/null || true; wait "$fpid" || true; fi
    /usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true
    # Reap guest stragglers (e.g. explorer.exe, UnityCrashHandler64) left in this prefix.
    for env in /proc/[0-9]*/environ; do
        pid=${env#/proc/}; pid=${pid%/environ}
        [[ $pid != "$$" && $pid != "$xpid" ]] || continue
        grep -qzx "WINEPREFIX=$WINEPREFIX" "$env" 2>/dev/null && kill -9 "$pid" 2>/dev/null || true
    done
    if [[ -n $server_pid ]]; then wait "$server_pid" || true; fi
    if [[ -n $xpid ]]; then kill "$xpid" 2>/dev/null || true; wait "$xpid" || true; fi
}
trap cleanup EXIT
rm -f "$work/display" "$work"/counters.* "$work/player.log" "$video" "$recording"
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$work/display" 9>&- >"$work/xvfb.log" 2>&1 &
xpid=$!
for ((i=0;i<100;i++)); do [[ -s $work/display ]] && break; sleep .05; done
export DISPLAY=:$(cat "$work/display")
xdpyinfo >/dev/null
ffmpeg -hide_banner -loglevel warning -y -f x11grab -framerate 10 -video_size 1280x720 -i "$DISPLAY" -t 58 -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$recording" >"$work/ffmpeg.log" 2>&1 9>&- &
fpid=$!
sleep .5
if ! kill -0 "$fpid" 2>/dev/null; then
    wait "$fpid" || true; fpid=
    cat "$work/ffmpeg.log" >&2
    echo 'Recording failed: ffmpeg exited during startup' >&2
    exit 1
fi
/usr/lib/wine/wineserver64 -f -p >"$work/server.log" 2>&1 9>&- &
server_pid=$!
export WINESERVER=/bin/false
printf '%s\n' 'constraints: wx=on; nofork=off (R4W in-process wineserver blocked); noproc=off (not supported by current harness)' 'THROUGH launcher -> Wine own W^X loader -> shim -> uwph selected calls; Linux, not Xbox device'
set +e
(cd "$game" && timeout --kill-after=2s 120s /workspace/xbox-readiness/bin/uwpjail --wx -- env LD_PRELOAD="$harness/out/libuwph_wine_shim.so" UWPH_REPORT="$work/counters" UWPH_REPORT_PID=1 "$harness/out/run_via_wine" "$game/Megabonk.exe" -force-d3d11 -screen-fullscreen 0 -logFile "Z:$work/player.log") >"$work/game.log" 2>&1 9>&-
rc=$?
set -e
sleep 2
python3 - "$work" "$harness/surface-trace.tsv" "$rc" <<'PY'
import pathlib, sys, datetime
work, trace, rc = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), sys.argv[3]
log = (work/'game.log').read_text(errors='replace')
player = (work/'player.log').read_text(errors='replace') if (work/'player.log').exists() else ''
hits = 0
with trace.open('a') as out:
    if trace.stat().st_size == 0:
        out.write('utc\trun\tpid\tmodule_hex\tsurface\twrapper_hits\tbackend_hits\n')
    for report in sorted(work.glob('counters.*')):
        for line in report.read_text().splitlines():
            fields = line.split()
            if fields[0] == 'module' and int(fields[3]):
                out.write('\t'.join([datetime.datetime.now(datetime.timezone.utc).isoformat(), 'B11-O1', report.suffix[1:], *fields[1:]])+'\n')
                hits += int(fields[4])
stage = 'loader'
if any('Loaded ' in line and 'UnityPlayer.dll' in line and 'native' in line for line in log.splitlines()):
    stage = 'unityplayer'
if 'GfxDevice: creating device client' in player:
    stage = 'gfx-init'
# First-frame/menu require reviewed visual evidence; loading DLLs does not prove rendering.
print(f'surface backend hits={hits}; game exit={rc}')
print(f'MEGABONK reached={stage}')
print('stage is a conservative log-proven lower bound; review video for first-frame/menu')
if 'SteamAPI_Init() failed' in player:
    print('first missing piece: SteamAPI_Init failed (Steam service/session unavailable); no DRM changes')
elif stage == 'loader':
    print('first missing piece: UnityPlayer load not observed; inspect game.log')
else:
    print('first missing piece: inspect game.log/player.log; no unproven rendering claim')
if not hits:
    raise SystemExit('No uwph backend hits; routed proof missing')
PY
cat "$work/game.log"
# Keep the guest-stage report above even when recording or validation fails.
recorder_rc=0
wait "$fpid" || recorder_rc=$?
fpid=
if ((recorder_rc != 0)); then
    cat "$work/ffmpeg.log" >&2
    echo "Recording failed: ffmpeg exit=$recorder_rc" >&2
    exit 1
fi
[[ -s $recording ]] || { echo 'Recording failed: empty or missing video' >&2; exit 1; }
ffprobe -v error -show_entries format=duration:stream=codec_type -of json "$recording" >"$work/video-probe.json"
python3 - "$work/video-probe.json" <<'PY'
import json, math, sys
probe = json.load(open(sys.argv[1]))
duration = float(probe.get('format', {}).get('duration', 'nan'))
if not math.isfinite(duration) or not 0 < duration <= 60:
    raise SystemExit(f'Recording failed: invalid duration {duration}')
if not any(s.get('codec_type') == 'video' for s in probe.get('streams', [])):
    raise SystemExit('Recording failed: no video stream')
print(f'video validated: duration={duration:.6f}s')
PY
# Decode the entire capture so a readable header alone cannot pass validation.
ffmpeg -v error -xerror -i "$recording" -map 0:v:0 -f null -
mv "$recording" "$video"
echo "video=$video"
# Reaching a bounded stage is a successful diagnostic; retain guest status above.
[[ -s $harness/surface-trace.tsv ]]
