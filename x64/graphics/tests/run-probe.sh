#!/usr/bin/env bash
# O4 Linux smoke only: probe_d3d12.exe via selected uwph routing + Wine's own
# PE loader, using Wine's builtin d3d12/dxgi (vkd3d) on lavapipe. Not Xbox.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(realpath ../..)
host=$root/x64/uwp-host/harness/out
make -C "$root/x64/uwp-host/harness" out/libuwph_wine_shim.so out/run_via_wine
make out/probe_d3d12.exe
ev=evidence/R4G-O4
mkdir -p out/o4 "$ev"
cp out/probe_d3d12.exe out/o4/
export WINEPREFIX=$PWD/out/prefix DISPLAY=${DISPLAY:-:99} WINEDEBUG=-all
[[ -d $WINEPREFIX ]] || { echo 'Missing initialized O2 prefix'; exit 1; }
export WINEDLLOVERRIDES='d3d12,d3d12core,dxgi=b;wineboot.exe=d'
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_DRIVER_FILES=$VK_ICD_FILENAMES
server=/usr/lib/wine/wineserver64
"$server" -f -p >"$ev/server.log" 2>&1 &
pid=$!
trap '"$server" -k >/dev/null 2>&1 || true; wait "$pid" || true' EXIT
export WINESERVER=/bin/false
log=$PWD/$ev/probe_d3d12.log
cd out/o4
sha256sum probe_d3d12.exe > "../../$ev/artifacts.sha256"
timeout --kill-after=2s 60s env LD_PRELOAD="$host/libuwph_wine_shim.so" \
    UWPH_REPORT="$root/x64/graphics/$ev/uwph.report" UWPH_REPORT_PID=1 \
    "$host/run_via_wine" "$PWD/probe_d3d12.exe" --log "Z:${log//\//\\}" "$@"
