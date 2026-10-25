#!/usr/bin/env bash
# Strict O3 proof: hello through launcher/Wine/shim/uwph, no real /proc.
set -euo pipefail
cd "$(dirname "$0")"
root=$(realpath ../../..)
make out/libuwph_wine_shim.so out/run_via_wine out/fake_proc_test
bwrap --ro-bind / / --tmpfs /proc -- "$PWD/out/fake_proc_test"
export WINEPREFIX=${UWPH_FIXTURE_PREFIX:-$PWD/out/wine-o4/prefix}
[[ -f $WINEPREFIX/system.reg ]] || { echo 'Initialized explicit fixture required' >&2; exit 1; }
export EXTRA_RW=$PWD/out WINEDEBUG=-all WINEDLLOVERRIDES=wineboot.exe=d
# Separate prestarted stock Linux wineserver is a retained concession.
/usr/lib/wine/wineserver64 -p 180 || true
export WINESERVER=/bin/false
wrapper=${UWPH_UWP_RUN:-/workspace/xbox-readiness/tests/uwp_run.sh}
log=$PWD/out/noproc.log
rc=0
timeout --kill-after=2s 20s "$wrapper" --noproc "$root" -- env \
  LD_PRELOAD="$PWD/out/libuwph_wine_shim.so" UWPH_REPORT="$PWD/out/noproc.report" \
  "$PWD/out/run_via_wine" "$root/x64/pe/tests/hello.exe" > "$log" 2>&1 || rc=$?
cat "$log"
[[ $rc == 0 ]]
grep -q 'TEB self-check: ok' "$log"
grep -q 'threads: shared=200000' "$log"
echo 'PASS hello THROUGH launcher -> Wine loader -> shim -> uwph; /proc is empty tmpfs'
