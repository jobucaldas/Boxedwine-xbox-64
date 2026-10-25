#!/usr/bin/env bash
# O3 Linux stand-in only, selected uwph routing + Wine's own PE loader.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(realpath ../..)
host=$root/x64/uwp-host/harness/out
make -C "$root/x64/uwp-host/harness" out/libuwph_wine_shim.so out/run_via_wine
mkdir -p out/o3 evidence/R4G-O3
# Local existing test dependencies; no download, prefix initialization or registration.
cp out/vulkan_clear.exe out/dcomp.dll out/o3/
for dll in vulkan_dzn.dll d3d12.dll d3d12core.dll dxil.dll libgcc_s_seh-1.dll libstdc++-6.dll; do
    cp "/workspace/xbox-readiness/dzn/runtime/$dll" out/o3/
done
export WINEPREFIX=$PWD/out/prefix DISPLAY=${DISPLAY:-:99} WINEDEBUG=-all
[[ -d $WINEPREFIX ]] || { echo 'Missing initialized O2 prefix'; exit 1; }
export WINEDLLOVERRIDES='dcomp,d3d12,d3d12core=n;wineboot.exe=d'
export VK_ICD_FILENAMES=/usr/share/vulkan/icd.d/lvp_icd.json
export VK_DRIVER_FILES=$VK_ICD_FILENAMES VKD3D_DEBUG=info
server=/usr/lib/wine/wineserver64
"$server" -f -p >evidence/R4G-O3/server.log 2>&1 &
pid=$!
trap '"$server" -k >/dev/null 2>&1 || true; wait "$pid" || true' EXIT
export WINESERVER=/bin/false
cd out/o3
sha256sum *.dll *.exe > ../../evidence/R4G-O3/artifacts.sha256
timeout --kill-after=2s 40s env LD_PRELOAD="$host/libuwph_wine_shim.so" \
    UWPH_REPORT="$root/x64/graphics/evidence/R4G-O3/uwph.report" UWPH_REPORT_PID=1 \
    "$host/run_via_wine" "$PWD/vulkan_clear.exe"
