#!/bin/bash
set -eu
lane=/workspace/x64-run/lanes/B10
h=/workspace/wt/B10/x64/uwp-host/harness
export WINEPREFIX=$lane/pfx
Xvfb -displayfd 3 -screen 0 1024x768x24 3>"$lane/display" >"$lane/xvfb.log" 2>&1 &
xpid=$!
trap 'WINEPREFIX="$lane/pfx" /usr/lib/wine/wineserver64 -k || true; kill "$xpid" || true' EXIT
for i in {1..50}; do test -s "$lane/display" && break; sleep .1; done
export DISPLAY=:$(cat "$lane/display")
/usr/lib/wine/wineserver64 -f -p >"$lane/server.log" 2>&1 &
export WINESERVER=/bin/false WINEDLLOVERRIDES=wineboot.exe=d
export WINE_LOADER=$h/out/wine-wx/lib/wine/wine64
export WINEDEBUG=+timestamp,+pid,+process,+module,+seh
set +e
timeout -k 3 120 strace -f -s 256 -e trace=process -o "$lane/install.exec" env LD_PRELOAD="$h/out/libuwph_wine_shim.so" UWPH_REPORT="$lane/install.counters" UWPH_REPORT_PID=1 "$h/out/run_via_wine" /workspace/steam-owned/SteamSetup.exe /S >"$lane/install.log" 2>&1
rc=$?
echo "$rc" >"$lane/install.exit"
find "$lane/pfx/drive_c" -iname steam.exe >"$lane/steam-path"
