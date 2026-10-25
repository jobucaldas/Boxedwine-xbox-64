#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
bash x64/launcher-dl/build.sh
out="$PWD/x64/launcher-dl/out"
run=$(mktemp -d "$out/anon-XXXXXXXX")
export WINEPREFIX="$out/wine-prefix"
export WINEDEBUG=-all
export WINEARCH=win64
# No existing Steam installation or account cache is used.
printf 'PROOF stock Wine; anonymous only; fresh output %s\n' "$run"
timeout 600 wine "$out/steamdl.exe" "Z:${run//\//\\}\\content"
