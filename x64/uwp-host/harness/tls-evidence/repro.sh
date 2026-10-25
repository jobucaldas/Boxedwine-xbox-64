#!/bin/bash
# Linux stand-in only; existing anonymous WinHTTP GetCMList probe, no credentials.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
E=${B44_EVIDENCE:-/workspace/x64-run/lanes/B44/o1}
assets=${B44_WINE_ASSETS:-$repo/x64/uwp-host/harness/out}
mkdir -p "$E" /workspace/x64-run/videos
E=$(realpath "$E")
cp /workspace/xbox-readiness/bin/net_probe.exe "$E/net_probe.exe"
cc -O2 -Wall -Wextra -Werror "$repo/x64/pe/launcher/run_via_wine.c" -o "$E/run_via_wine"
for n in $(seq 250 299); do
 if [[ ! -e /tmp/.X11-unix/X$n && ! -e /tmp/.X$n-lock ]]; then break; fi
done
export DISPLAY=:$n
Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp > "$E/xvfb.log" 2>&1 & xp=$!
trap 'kill "$xp" 2>/dev/null || true' EXIT
sleep 1
ffmpeg -y -loglevel error -f x11grab -video_size 1024x768 -framerate 10 -i "$DISPLAY" -t 55 -c:v libx264 -threads 1 -pix_fmt yuv420p /workspace/x64-run/videos/B44-O1.mp4 > "$E/ffmpeg.log" 2>&1 & vp=$!
printf 'B44 O1 Linux stand-in: launcher -> Wine own loader -> uwph\nNormal and process-local UWPH_DENY_DL=gnutls\nNo Xbox console PASS claim\n' > "$E/video.txt"
xmessage -geometry 1000x240+5+5 -file "$E/video.txt" >/dev/null 2>&1 &
for mode in normal deny; do
 if [[ -e "$E/prefix-$mode" ]]; then mv "$E/prefix-$mode" "$E/prefix-$mode.previous-$(date +%s)"; fi
 cp -a --reflink=auto /workspace/megabonk/pfx-stock "$E/prefix-$mode"
 export WINEPREFIX="$E/prefix-$mode" WINEDLLOVERRIDES=wineboot.exe=d WINEDEBUG=+winhttp,+schannel,+loaddll
 /usr/lib/wine/wineserver64 -f -p > "$E/$mode-server.log" 2>&1 & sp=$!
 sleep .3
 deny=''; [[ $mode != deny ]] || deny=gnutls
 set +e
 WINESERVER=/bin/false WINE_LOADER="$assets/wine-wx/lib/wine/wine64" \
 timeout -k 3 22 strace -f -s 256 -e trace=process -o "$E/$mode.exec" \
 env UWPH_DENY_DL="$deny" LD_PRELOAD="$repo/x64/uwp-host/harness/out/libuwph_wine_shim.so" UWPH_REPORT="$E/$mode.counters" UWPH_REPORT_PID=1 \
 "$E/run_via_wine" "$E/net_probe.exe" > "$E/$mode.log" 2> "$E/$mode-debug.log"
 rc=$?
 set -e
 echo "$rc" > "$E/$mode.exit"
 /usr/lib/wine/wineserver64 -k || true
 wait "$sp" || true
 xmessage -geometry 1000x420+5+260 -file "$E/$mode.log" >/dev/null 2>&1 &
done
wait "$vp"
sha256sum "$E/net_probe.exe" "$E/run_via_wine" "$repo/x64/uwp-host/harness/out/libuwph_wine_shim.so" "$assets/wine-wx/lib/wine/wine64" > "$E/artifacts.sha256"
