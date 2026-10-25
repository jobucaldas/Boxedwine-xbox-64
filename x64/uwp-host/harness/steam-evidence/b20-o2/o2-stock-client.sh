#!/bin/bash
# CONTROL ONLY: stock Debian `wine` (old-style i386 process), stock-installed prefix, 120 s, no login.
L=/workspace/x64-run/lanes/B20
export WINEPREFIX=/workspace/wt/B20/x64/uwp-host/harness/out/b20/pfx-wow64
Xvfb :123 -screen 0 1024x768x24 >/dev/null 2>&1 & xp=$!; sleep 1; export DISPLAY=:123
WINEDEBUG=+timestamp,+process,err+all timeout -k 5 120 wine "C:\\Program Files (x86)\\Steam\\Steam.exe" -silent -no-browser >"$L/o2-stock-client.log" 2>&1
echo "rc=$?" >"$L/o2-stock-client.exit"
wineserver -k; wineserver -w; kill $xp
