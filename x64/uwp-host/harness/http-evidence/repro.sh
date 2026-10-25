#!/bin/bash
# B30 O1 only: anonymous WinINet GET, no Steam execution.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
E=${B30_EVIDENCE:-/workspace/x64-run/lanes/B30/o1}
assets=${B30_WINE_ASSETS:-/workspace/wt/B31/x64/uwp-host/harness/out}
mkdir -p "$E" /workspace/x64-run/videos
E=$(realpath "$E")
[[ ! -e "$E/prefix-stock" && ! -e "$E/prefix-uwph" ]] || { echo 'Use a fresh B30_EVIDENCE directory' >&2; exit 1; }
python3 - "$E" <<'PY'
from pathlib import Path
import sys
b = Path('/workspace/x64-run/lanes/B22/o2/run.0L0qbguI')
lines = ['B30 O1: verbatim B22 evidence; anonymous bootstrap GET']
lines += [x for x in (b/'bootstrap_log.txt').read_text().splitlines() if 'Downloading manifest:' in x][:1]
lines += [x for x in (b/'client.log').read_text().splitlines() if 'err:wininet:open_http_connection' in x or 'connect failed, status' in x]
(Path(sys.argv[1])/'source-evidence.txt').write_text('\n'.join(lines)+'\n')
PY
i686-w64-mingw32-gcc -O2 -Wall -Wextra -Werror "$repo/x64/on-device/readiness/http_probe.c" -o "$E/http_probe.exe" -lwininet
cc -O2 -Wall -Wextra -Werror "$repo/x64/pe/launcher/run_via_wine.c" -o "$E/run_via_wine"
for n in $(seq 210 249); do
 if [[ ! -e /tmp/.X11-unix/X$n && ! -e /tmp/.X$n-lock ]]; then break; fi
done
export DISPLAY=:$n
Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp > "$E/xvfb.log" 2>&1 & xp=$!
trap 'kill "$xp" 2>/dev/null || true' EXIT
sleep 1
ffmpeg -y -loglevel error -f x11grab -video_size 1024x768 -framerate 10 -i "$DISPLAY" -t 55 -c:v libx264 -threads 1 -pix_fmt yuv420p /workspace/x64-run/videos/B30-O1.mp4 > "$E/ffmpeg.log" 2>&1 & vp=$!
xmessage -geometry 980x650+10+10 -file "$E/source-evidence.txt" > /dev/null 2>&1 &
mkdir -p "$E/wow64-dlls"
ln -sfn /usr/lib/i386-linux-gnu/wine/i386-windows "$E/wow64-dlls/i386-windows"
ln -sfn "$assets/wine-wx/lib/x86_64-linux-gnu/wine/x86_64-unix" "$E/wow64-dlls/x86_64-unix"
for mode in stock uwph; do
 cp -a --reflink=auto /workspace/megabonk/pfx-stock "$E/prefix-$mode"
 export WINEPREFIX="$E/prefix-$mode" WINEDLLOVERRIDES=wineboot.exe=d WINEDEBUG=+wininet,+winsock,+loaddll
 strace -f -yy -s 256 -e trace=process,network -o "$E/$mode-server.strace" /usr/lib/wine/wineserver64 -f -p > "$E/$mode-server.log" 2>&1 & sp=$!
 sleep .3
 set +e
 if [[ $mode == stock ]]; then
  timeout -k 3 40 /usr/lib/wine/wine64 "$E/http_probe.exe" > "$E/stock.log" 2> "$E/stock-debug.log"
 else
  WINESERVER=/bin/false WINE_LOADER="$assets/wine-wx/lib/wine/wine64" UWPH_WOW64_DLL_ROOT="$E/wow64-dlls" \
  timeout -k 3 40 strace -f -s 256 -yy -e trace=process,network -o "$E/uwph.strace" \
  env LD_PRELOAD="$repo/x64/uwp-host/harness/out/libuwph_wine_shim.so" UWPH_REPORT="$E/uwph.counters" UWPH_REPORT_PID=1 \
  "$E/run_via_wine" "$E/http_probe.exe" > "$E/uwph.log" 2> "$E/uwph-debug.log"
 fi
 rc=$?
 set -e
 echo "$rc" > "$E/$mode.exit"
 /usr/lib/wine/wineserver64 -k || true
 wait "$sp" || true
 rg '^(GET |CALL |FAIL |HTTP |BODY |COMPLETE )' "$E/$mode.log" > "$E/$mode-output.txt" || true
 xmessage -geometry 980x250+10+350 -file "$E/$mode-output.txt" > /dev/null 2>&1 &
done
wait "$vp"
python3 "$repo/x64/uwp-host/harness/http-evidence/validate.py" "$E"
