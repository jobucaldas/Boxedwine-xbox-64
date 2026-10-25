#!/usr/bin/env bash
set -euo pipefail
[[ ${1:-} == --stock && ${2:-} == --evidence && -n ${3:-} && $# == 3 ]] || { echo 'usage: run-https-probe.sh --stock --evidence DIR' >&2; exit 2; }
evidence=$(mkdir -p "$3" && cd "$3" && pwd)
source_dir=$(cd "$(dirname "$0")" && pwd)
prefix=$(mktemp -d /tmp/B70-wine-XXXXXX)
export WINEPREFIX=$prefix WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml='
cleanup() { wineserver -k 2>/dev/null || true; rm -rf "$prefix"; }
trap cleanup EXIT
x86_64-w64-mingw32-gcc -O2 -Wall -Wextra "$source_dir/https_probe.c" -o "$evidence/https_probe.exe" -lwinhttp -lcrypt32 -liphlpapi -lws2_32 2>"$evidence/build.log"
wine --version > "$evidence/environment.log"
timeout 60 wineboot -u > "$evidence/wineboot.log" 2>&1
# The earlier readiness executable is ours; rerun in this same fresh prefix if available.
if [[ -f /workspace/xbox-readiness/bin/net_probe.exe ]]; then
    timeout 90 wine /workspace/xbox-readiness/bin/net_probe.exe > "$evidence/readiness-net.log" 2>&1 || true
fi
timeout 90 wine "$evidence/https_probe.exe" 2>"$evidence/wine-stderr.log" | tr -d '\r' | tee "$evidence/https_probe.log"
du -sh "$prefix" >> "$evidence/environment.log"
