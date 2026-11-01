#!/bin/bash
# R3 prototype runner: everything here runs under distro Wine on Linux, headless (Xvfb),
# with no Steam account. Inputs are fetched separately (see NOTES-x64.md, step R3):
#   R3_DXVK   = extracted dxvk-<ver>            (github.com/doitsujin/dxvk releases)
#   R3_VKD3D  = extracted vkd3d-proton-<ver>    (github.com/HansKristian-Work/vkd3d-proton releases)
#   R3_VALHEIM = steamcmd anonymous app 896660 (Valheim Dedicated Server), Windows depot
# Usage: x64/pe/r3/run_r3.sh [all|d3d|steamapi|valheim]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
: "${R3_DXVK:=/workspace/r3-dl/dxvk-3.1.1}"
: "${R3_VKD3D:=/workspace/r3-dl/vkd3d-proton-3.0.1}"
: "${R3_VALHEIM:=/workspace/r3-games/valheim-ds}"
: "${R3_OUT:=/tmp/r3-out}"
export WINEPREFIX="${R3_PREFIX:-$HOME/.wine-r3}"
export WINEDEBUG=-all
export DISPLAY="${R3_DISPLAY:-:93}"
BASE_OVR="mscoree,mshtml="
DX_OVR="d3d11,dxgi,d3d10core,d3d9,d3d8,d3d12,d3d12core=n"
mkdir -p "$R3_OUT"
what="${1:-all}"

if ! xdpyinfo -display "$DISPLAY" >/dev/null 2>&1; then
    Xvfb "$DISPLAY" -screen 0 1024x768x24 >"$R3_OUT/xvfb.log" 2>&1 &
    sleep 2
fi

if [ ! -f "$WINEPREFIX/system.reg" ]; then
    echo "== creating prefix $WINEPREFIX"
    WINEDLLOVERRIDES="$BASE_OVR" wineboot -i >"$R3_OUT/wineboot.log" 2>&1
fi

# Waits for a non-black frame in the top-left 680x260 and saves it.
shoot() {
    for _ in $(seq 1 40); do
        sleep 0.5
        xwd -root -display "$DISPLAY" -silent | convert xwd:- -crop 680x260+0+0 "$R3_OUT/.shot.png" 2>/dev/null || continue
        m=$(convert "$R3_OUT/.shot.png" -format '%[fx:mean]' info:)
        if awk "BEGIN{exit !($m>0.02)}"; then mv "$R3_OUT/.shot.png" "$1"; echo "   screenshot $1"; return; fi
    done
    echo "   (no non-black frame captured for $1)"
}

run_d3d() {
    local S="$WINEPREFIX/drive_c/windows/system32"
    echo "== D3D11 / D3D12 with Wine builtins (wined3d, vkd3d)"
    WINEDLLOVERRIDES="$BASE_OVR" timeout 120 wine "$HERE/d3d11_probe.exe" 2>&1 | tee "$R3_OUT/d3d11-builtin.log"
    WINEDLLOVERRIDES="$BASE_OVR" timeout 120 wine "$HERE/d3d12_probe.exe" 2>&1 | tee "$R3_OUT/d3d12-builtin.log"

    echo "== installing DXVK + VKD3D-Proton into the prefix (native override, as Proton/GameNative do)"
    for d in d3d11 dxgi d3d10core d3d9 d3d8; do cp -f "$R3_DXVK/x64/$d.dll" "$S/"; done
    for d in d3d12 d3d12core; do cp -f "$R3_VKD3D/x64/$d.dll" "$S/"; done

    echo "== D3D11 via DXVK"
    WINEDLLOVERRIDES="$BASE_OVR;$DX_OVR" DXVK_LOG_LEVEL=info DXVK_LOG_PATH=none timeout 180 \
        wine "$HERE/d3d11_probe.exe" >"$R3_OUT/d3d11-dxvk.log" 2>&1 &
    shoot "$R3_OUT/d3d11-dxvk.png"; wait
    grep -E '^(d3d11|feature|adapter|readback)|DXVK: v|Found device' "$R3_OUT/d3d11-dxvk.log"

    echo "== D3D12 via VKD3D-Proton"
    WINEDLLOVERRIDES="$BASE_OVR;$DX_OVR" VKD3D_DEBUG=warn timeout 180 \
        wine "$HERE/d3d12_probe.exe" >"$R3_OUT/d3d12-vkd3dp.log" 2>&1 &
    shoot "$R3_OUT/d3d12-vkd3dp.png"; wait
    grep -E '^(d3d12|adapter|presented)|applicationVersion' "$R3_OUT/d3d12-vkd3dp.log"
}

run_steamapi() {
    echo "== Steamworks client API probe (Valve steam_api64.dll, no Steam client running)"
    local d="$R3_OUT/sapi"; mkdir -p "$d"; echo 480 >"$d/steam_appid.txt"
    (cd "$d" && WINEDLLOVERRIDES="$BASE_OVR" timeout 60 wine "$HERE/steamapi_probe.exe" \
        "$(winepath -w "$R3_VALHEIM/valheim_server_Data/Plugins/x86_64")" 2>&1 | tee "$R3_OUT/steamapi.log")
}

run_valheim() {
    echo "== Valheim Dedicated Server (anonymous steamcmd download), headless, 240 s"
    local save="$WINEPREFIX/drive_c/r3save"; rm -rf "$save"; mkdir -p "$save"
    (cd "$R3_VALHEIM" && SteamAppId=892970 WINEDLLOVERRIDES="$BASE_OVR" WINEDEBUG=-all,+loaddll timeout 240 \
        wine ./valheim_server.exe -nographics -batchmode -name R3probe -port 2456 -world R3world \
        -password r3secret -public 0 -savedir 'C:\r3save' -logFile 'C:\r3save\valheim.log' \
        >"$R3_OUT/valheim-wine.log" 2>&1)
    echo "   exit=$? (124 = stopped by the 240 s timeout, i.e. still running)"
    cp "$save/valheim.log" "$R3_OUT/valheim.log"
    grep -E 'Initialize engine|steam APPID|Steam game server initialized|Load world|Opened Steam server|Game server connected|Loading: Done' "$R3_OUT/valheim.log"
    grep -iE 'loaddll.*(valheim_server.exe|steam_api64|steamclient64|UnityPlayer)' "$R3_OUT/valheim-wine.log" | sed 's/^.*build_module //'
}

case "$what" in
    d3d) run_d3d ;;
    steamapi) run_steamapi ;;
    valheim) run_valheim ;;
    all) run_d3d; run_steamapi; run_valheim ;;
    *) echo "usage: $0 [all|d3d|steamapi|valheim]"; exit 2 ;;
esac
