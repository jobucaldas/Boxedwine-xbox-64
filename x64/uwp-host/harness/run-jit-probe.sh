#!/usr/bin/env bash
# R4X O3, Linux harness only. Runs the readiness jit_probe.exe through our
# launcher -> W^X Wine loader/ntdll.so -> shim -> uwph. The RW->RX JIT pattern
# must pass; the probe's PAGE_EXECUTE_READWRITE requests must be refused.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
pe=${UWPH_JIT_PROBE:-/workspace/xbox-readiness/bin/jit_probe.exe}
work=$PWD/wx-jit
mkdir -p "$work"
exec 9>"$work/lock"
flock -n 9
if [[ ! -d $work/prefix ]]; then
    cp -a --reflink=auto /workspace/megabonk/pfx-stock "$work/prefix"
fi
export WINEPREFIX=$work/prefix WINEDLLOVERRIDES=wineboot.exe=d WINEDEBUG=${WINEDEBUG:--all}
/usr/lib/wine/wineserver64 -f -p >"$work/server.log" 2>&1 &
server_pid=$!
trap '/usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true; wait "$server_pid" || true' EXIT
export WINESERVER=/bin/false WINE_LOADER=${WINE_LOADER:-$PWD/wine-wx/lib/wine/wine64}
echo "jit_probe: $pe sha256=$(sha256sum "$pe" | cut -d' ' -f1)"
rm -f "$work"/counters*
set +e
timeout --kill-after=2s 15s env LD_PRELOAD="$PWD/libuwph_wine_shim.so" UWPH_REPORT="$work/counters" UWPH_REPORT_PID=1 ./run_via_wine "$pe" | tee "$work/probe.raw.log"
rc=${PIPESTATUS[0]}
set -e
tr -d '\r' <"$work/probe.raw.log" >"$work/probe.log"
cat "$work"/counters* >"$work/counters.all" 2>/dev/null || :
read -r protect rwx < <(awk '
    $1=="total" && $2=="mprotect" { p+=$3 }
    $1=="module" && ($3=="mmap" || $3=="mprotect") { r+=$4-$5 }
    END { print p+0, r+0 }' "$work/counters.all")
summary=$(grep -m1 '^SUMMARY jit_probe ' "$work/probe.log" || :)
rwx_alloc=$(sed -n 's/^RWX_ALLOC=//p' "$work/probe.log")
rwx_protect=$(sed -n 's/^RWX_PROTECT=//p' "$work/probe.log")
echo "W^X: uwph_protect backend calls=$protect, refused RWX mmap/mprotect requests=$rwx"
echo "THROUGH launcher -> Wine own loader -> uwph selected calls: exit=$rc"
# Legal path: every CHECK (RW alloc, RW->RX flips, patch cycles, dual mapping) passed.
legal=FAIL
[[ $rc == 0 && $summary == *" fail=0" && $protect -gt 0 ]] && legal=PASS
# Illegal path: both RWX requests failed in the PE and the shim saw the refusal.
illegal=FAIL
[[ $rwx_alloc == 0 && $rwx_protect == 0 && $rwx -gt 0 ]] && illegal=REFUSED
echo "R4X O3: RW->RX $legal (${summary:-no SUMMARY}); RWX $illegal (RWX_ALLOC=${rwx_alloc:-?} RWX_PROTECT=${rwx_protect:-?})"
[[ $legal == PASS && $illegal == REFUSED ]] || exit $(( rc ? rc : 3 ))
