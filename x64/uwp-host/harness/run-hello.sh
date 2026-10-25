#!/usr/bin/env bash
# Linux harness only. Wine owns PE mapping/relocations; uwph owns routed calls.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
root=$(realpath ../../../..)
work=$PWD/wx-hello
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
rm -f "$work"/counters*
set +e
timeout --kill-after=2s 15s env LD_PRELOAD="$PWD/libuwph_wine_shim.so" UWPH_REPORT="$work/counters" UWPH_REPORT_PID=1 ./run_via_wine "$root/x64/pe/tests/hello.exe"
rc=$?
set -e
for report in "$work"/counters*; do [[ ! -f $report ]] || cat "$report"; done
# W^X evidence: protect transitions reached the uwph backend; RWX mmap requests
# show up as wrapper calls without a backend entry (refused with EACCES).
read -r protect rwx < <(cat "$work"/counters* | awk '
    $1=="total" && $2=="mprotect" { p+=$3 }
    $1=="module" && $3=="mmap" { r+=$4-$5 }
    END { print p+0, r+0 }')
echo "W^X: uwph_protect backend calls=$protect, refused RWX mmap requests=$rwx"
echo "THROUGH launcher -> Wine own loader -> uwph selected calls: exit=$rc"
if [[ $rc == 0 && ( $protect == 0 || $rwx != 0 ) ]]; then echo "W^X evidence missing" >&2; exit 3; fi
exit "$rc"
