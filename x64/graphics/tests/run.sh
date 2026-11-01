#!/usr/bin/env bash
# Linux stand-in: launcher + selected Unix-side uwph routing, not UWP proof.
set -euo pipefail
cd "$(dirname "$0")/.."
root=$(realpath ../..)
host=$root/x64/uwp-host/harness/out
mkdir -p out "$host"
make -C "$root/x64/uwp-host/harness" out/libuwph_wine_shim.so out/run_via_wine
prefix=$PWD/out/prefix
if [[ ! -d $prefix ]]; then
    cp -a --reflink=auto "${UWPH_FIXTURE_PREFIX:-/workspace/megabonk/pfx-stock}" "$prefix"
fi
export WINEPREFIX=$prefix DISPLAY=${DISPLAY:-:99} WINEDEBUG=-all
export WINEDLLOVERRIDES='dcomp=n;wineboot.exe=d'
server=/usr/lib/wine/wineserver64
"$server" -f -p >out/server.log 2>&1 &
pid=$!
trap '"$server" -k >/dev/null 2>&1 || true; wait "$pid" || true' EXIT
export WINESERVER=/bin/false
# timeout itself is not preloaded; only our launcher and Wine loader are.
timeout --kill-after=2s 30s env LD_PRELOAD="$host/libuwph_wine_shim.so" \
    UWPH_REPORT="$PWD/out/uwph.report" UWPH_REPORT_PID=1 \
    "$host/run_via_wine" "$PWD/out/test_dcomp.exe"
