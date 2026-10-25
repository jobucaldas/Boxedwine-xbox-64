#!/bin/bash
# Diagnostic only: original B20 installer DLL path, no overlay/fix.
set -eu
repo=$(cd "$(dirname "$0")/../../../../.." && pwd)
assets=${B20_HARNESS:-/workspace/wt/B20/x64/uwp-host/harness}
E=/workspace/x64-run/lanes/B22/o1
mkdir -p "$E" /workspace/x64-run/videos
pfx=$assets/out/b20/pfx-wow64-uwph
test -f "$pfx/system.reg"
i686-w64-mingw32-gcc -O2 "$repo/x64/on-device/readiness/net_probe.c" -o "$E/net_probe-i386.exe" -lws2_32
file "$E/net_probe-i386.exe" > "$E/probe.file"
cc -O2 "$repo/x64/pe/launcher/run_via_wine.c" -o "$E/run_via_wine" -ldl
# Select an unused X server socket, with -nolisten tcp.
for n in $(seq 130 199); do
    if [[ ! -e /tmp/.X11-unix/X$n && ! -e /tmp/.X$n-lock ]]; then break; fi
done
export DISPLAY=:$n
Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp > "$E/xvfb.log" 2>&1 & xp=$!
export WINEPREFIX=$pfx
cleanup() { env -u LD_PRELOAD /usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true; kill "$xp" 2>/dev/null || true; }
trap cleanup EXIT
sleep 1
ffmpeg -y -loglevel error -f x11grab -video_size 1024x768 -framerate 10 -i "$DISPLAY" -t 12 -c:v libx264 -pix_fmt yuv420p /workspace/x64-run/videos/B22-O1.mp4 > "$E/ffmpeg.log" 2>&1 & vp=$!
# Visible title states what the terminal-free DLL-init capture represents.
if command -v xmessage >/dev/null; then
    xmessage -geometry 920x180+30+30 'B22 O1: i386 net_probe THROUGH uwph / Wine own loader. Diagnostic: WS2_32 PROCESS_ATTACH fails before main. WINEDEBUG=+winsock,+module,+loaddll. Full call and path evidence: /workspace/x64-run/lanes/B22/o1/probe.log' > /dev/null 2>&1 &
fi
env -u LD_PRELOAD /usr/lib/wine/wineserver64 -f -p > "$E/server.log" 2>&1 & sp=$!
sleep .3
set +e
WINESERVER=/bin/false WINEDLLOVERRIDES=wineboot.exe=d \
 WINE_LOADER=$assets/out/wine-wx/lib/wine/wine64 \
 WINEDLLPATH=/usr/lib/i386-linux-gnu/wine \
 WINEDEBUG=+winsock,+module,+loaddll \
 timeout -k 5 25 strace -f -s 256 -e trace=process,openat,socket -o "$E/probe.strace" \
 env LD_PRELOAD="$assets/out/libuwph_wine_shim.so" UWPH_REPORT="$E/probe.counters" UWPH_REPORT_PID=1 \
 "$E/run_via_wine" "$E/net_probe-i386.exe" > "$E/probe.log" 2>&1
rc=$?
set -e
printf '%s\n' "$rc" > "$E/probe.exit"
env -u LD_PRELOAD /usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true
wait "$sp" || true
wait "$vp"
printf 'THROUGH-uwph probe exit=%s; evidence=%s\n' "$rc" "$E"
