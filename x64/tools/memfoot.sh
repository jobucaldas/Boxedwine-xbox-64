#!/usr/bin/env bash
# Linux THROUGH-uwph measurement; no stock-only control or Xbox budget proof.
set -euo pipefail
root=$(cd "$(dirname "$0")/../.." && pwd)
harness=$root/x64/uwp-host/harness
mkdir -p "$harness/out"
make -s -C "$harness" out/libuwph_wine_shim.so out/run_via_wine out/wine-wx/.stamp
exec python3 "$root/x64/tools/memfoot.py" "$root"
