#!/bin/bash
# B20 O2 step 2: THROUGH uwph. Our launcher (run_via_wine) -> Wine's own private W^X
# loader (out/wine-wx, no 32-bit wine sibling => new-style in-process WoW64) with the
# uwph shim preloaded. WINEDLLPATH supplies Debian's i386-windows PE dlls (B20 O1).
# Phase A: SteamSetup.exe /S into a fresh prefix. Phase B: Steam.exe -silent -no-browser, 180 s.
# No login, no token access, no Steam patching.
set -u
L=/workspace/x64-run/lanes/B20
h=/workspace/wt/B20/x64/uwp-host/harness
E=$h/out/b20/o2
mkdir -p "$E"
export DISPLAY=${DISPLAY:?}
# WoW64 dll overlay (symlinks only): ntdll.so resolves a 32-bit builtin's unix side as
# <dir>/x86_64-unix/<name>.so next to <dir>/i386-windows; Debian's i386 tree has no
# x86_64-unix, so 32-bit crypt32.dll failed (run 1, client.log). Xbox package layout = this.
D=$h/out/b20/wow64-dlls
mkdir -p "$D"
ln -sfn /usr/lib/i386-linux-gnu/wine/i386-windows "$D/i386-windows"
ln -sfn "$h/out/wine-wx/lib/x86_64-linux-gnu/wine/x86_64-unix" "$D/x86_64-unix"
DLLPATH=${O2_DLLPATH:-$D}
T=${O2_TAG:-}
server=/usr/lib/wine/wineserver64
route() {  # route <tag> <prefix> <timeout> <exe> [args...]
    local tag=$1 pfx=$2 t=$3; shift 3
    export WINEPREFIX=$pfx
    env -u LD_PRELOAD "$server" -f -p >"$E/$tag.server.log" 2>&1 & local sp=$!
    sleep .3
    WINESERVER=/bin/false WINEDLLOVERRIDES=wineboot.exe=d \
    WINE_LOADER=$h/out/wine-wx/lib/wine/wine64 WINEDLLPATH=$DLLPATH \
    WINEDEBUG=+timestamp,+pid,+process,+module,+seh,err+all \
    timeout -k 5 "$t" strace -f -s 256 -e trace=process -o "$E/$tag.exec" \
        env LD_PRELOAD="$h/out/libuwph_wine_shim.so" UWPH_REPORT="$E/$tag.counters" UWPH_REPORT_PID=1 \
        "$h/out/run_via_wine" "$@" >"$E/$tag.log" 2>&1
    echo $? >"$E/$tag.exit"
    env -u LD_PRELOAD "$server" -k >/dev/null 2>&1; wait $sp 2>/dev/null
    echo "[$tag] exit=$(cat "$E/$tag.exit")"
}
P=$h/out/b20/pfx-wow64-uwph
if [[ ! -f $P/system.reg ]]; then
    echo "SETUP ONLY (stock wineboot -i, not evidence): fresh prefix $P"
    WINEPREFIX=$P WINEDEBUG=-all wineboot -i >/dev/null 2>&1; WINEPREFIX=$P wineserver -w
fi
if [[ -z ${O2_SKIP_INSTALL:-} ]]; then
echo "== Phase A: SteamSetup.exe /S THROUGH launcher -> W^X loader -> uwph"
route install "$P" 180 /workspace/steam-owned/SteamSetup.exe /S
find "$P/drive_c" -iname steam.exe -ls | tee "$E/install.steam-path"
fi
S=$(find "$P/drive_c" -iname steam.exe | head -1); src=uwph-installed
if [[ -z $S ]]; then P=$h/out/b20/pfx-wow64; S=$(find "$P/drive_c" -iname steam.exe | head -1); src=stock-installed; fi
echo "$src $S" >"$E/client$T.source"
echo "== Phase B: $src Steam.exe -silent -no-browser THROUGH uwph (180 s cap)"
echo "WINEDLLPATH=$DLLPATH"
route client$T "$P" 180 "$S" -silent -no-browser
echo "== done"
