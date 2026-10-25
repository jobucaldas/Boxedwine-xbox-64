#!/bin/bash
# B20 O2 step 1: STOCK Wine (default Debian `wine`, outside uwpjail) silent Steam install into the O1 prefix.
set -u
L=/workspace/x64-run/lanes/B20
export WINEPREFIX=/workspace/wt/B20/x64/uwp-host/harness/out/b20/pfx-wow64
Xvfb :121 -screen 0 1024x768x24 >"$L/o2-stock-xvfb.log" 2>&1 & xp=$!
sleep 1; export DISPLAY=:121 WINEDEBUG=+timestamp,+process,err+all
timeout -k 5 300 wine /workspace/steam-owned/SteamSetup.exe /S >"$L/o2-stock-install.log" 2>&1
echo "rc=$?" >"$L/o2-stock-install.exit"
wineserver -w; kill $xp
find "$WINEPREFIX/drive_c" -iname steam.exe -ls >>"$L/o2-stock-install.exit"
