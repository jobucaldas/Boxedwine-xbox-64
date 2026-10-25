#!/bin/bash
# THROUGH uwph: paired WoW64 layout; no Steam modifications or login.
set -eu
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
assets=${B20_HARNESS:-/workspace/wt/B20/x64/uwp-host/harness}
mkdir -p /workspace/x64-run/lanes/B22/o2 /workspace/x64-run/videos
E=$(mktemp -d /workspace/x64-run/lanes/B22/o2/run.XXXXXXXX)
pfx=$assets/out/b20/pfx-wow64-uwph
test -f "$pfx/system.reg"
i686-w64-mingw32-gcc -O2 "$repo/x64/on-device/readiness/net_probe.c" -o "$E/net_probe-i386.exe" -lws2_32
file "$E/net_probe-i386.exe" > "$E/probe.file"
cc -O2 -Wall -Wextra -Werror "$repo/x64/pe/launcher/run_via_wine.c" -o "$E/run_via_wine" -ldl
# Select an unused X server socket, with -nolisten tcp.
for n in $(seq 130 199); do
    if [[ ! -e /tmp/.X11-unix/X$n && ! -e /tmp/.X$n-lock ]]; then break; fi
done
export DISPLAY=:$n
Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp > "$E/xvfb.log" 2>&1 & xp=$!
D=$E/wow64-dlls
mkdir -p "$D"
ln -sfn /usr/lib/i386-linux-gnu/wine/i386-windows "$D/i386-windows"
ln -sfn "$assets/out/wine-wx/lib/x86_64-linux-gnu/wine/x86_64-unix" "$D/x86_64-unix"
export UWPH_WOW64_DLL_ROOT=$D
export WINEPREFIX=$pfx
cleanup() { env -u LD_PRELOAD /usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true; kill "$xp" 2>/dev/null || true; }
trap cleanup EXIT
sleep 1
ffmpeg -y -loglevel error -use_wallclock_as_timestamps 1 -f x11grab -video_size 1024x768 -framerate 10 -i "$DISPLAY" -t 55 -r 10 -c:v libx264 -threads 1 -pix_fmt yuv420p /workspace/x64-run/videos/B22-O2.mp4 > "$E/ffmpeg.log" 2>&1 & vp=$!
if command -v xmessage >/dev/null; then
    xmessage -geometry 920x100+30+30 'B22 O2: Steam.exe -no-browser -silent THROUGH uwph and Wine own WoW64 loader. Paired DLL layout; W^X enforced. No login. Bootstrap evidence in /workspace/x64-run/lanes/B22/o2.' >/dev/null 2>&1 &
fi
env -u LD_PRELOAD /usr/lib/wine/wineserver64 -f -p > "$E/server.log" 2>&1 & sp=$!
sleep .3
route() {
 local tag=$1 cap=$2; shift 2
 set +e
 WINESERVER=/bin/false WINEDLLOVERRIDES=wineboot.exe=d \
 WINE_LOADER=$assets/out/wine-wx/lib/wine/wine64 \
 WINEDEBUG=+winsock,+module,+loaddll,+process \
 timeout -k 5 "$cap" strace -f -s 256 -e trace=process,openat,socket -o "$E/$tag.strace" \
 env LD_PRELOAD="$assets/out/libuwph_wine_shim.so" UWPH_REPORT="$E/$tag.counters" UWPH_REPORT_PID=1 \
 "$E/run_via_wine" "$@" > "$E/$tag.log" 2>&1
 rc=$?
 set -e
 printf '%s\n' "$rc" > "$E/$tag.exit"
}
route probe 20 "$E/net_probe-i386.exe"
bootstrap="$pfx/drive_c/Program Files (x86)/Steam/logs/bootstrap_log.txt"
if [[ -f "$bootstrap" ]]; then cp "$bootstrap" "$E/bootstrap.before"; else : > "$E/bootstrap.before"; fi
printf '%s\n' "$pfx" > "$E/prefix.txt"
route client 90 "$pfx/drive_c/Program Files (x86)/Steam/Steam.exe" -no-browser -silent
env -u LD_PRELOAD /usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true
wait "$sp" || true
wait "$vp"
python3 "$(dirname "$0")/summarize.py" "$E" "$bootstrap"
printf 'THROUGH-uwph evidence=%s\n' "$E"
