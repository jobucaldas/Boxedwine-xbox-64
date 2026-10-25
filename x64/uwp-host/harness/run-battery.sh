#!/usr/bin/env bash
# B31 O1: the xbox-readiness probe battery THROUGH our launcher -> Wine's own
# PE loader (private W^X wine64/ntdll) -> shim -> uwph, under each uwpjail
# constraint set. Same fixture/server/shim setup as run-acceptance.sh.
# Usage: run-battery.sh [CONSTRAINTS...]   (default: every set below)
# stdout: one TSV row per (set, probe): constraints probe exit report_traced_count
# Diagnostics and PASS/FAIL summaries go to stderr.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
root=$(realpath ../../..)
make -s out/libuwph_wine_shim.so out/run_via_wine >&2
bin=${UWPH_BATTERY_BIN:-/workspace/xbox-readiness/bin}
probes=(hello seh_probe thread_probe fs_probe net_probe jit_probe)
sets=("$@")
(( ${#sets[@]} )) || sets=(nofork net cpu6 noproc appfs wx nofork,noproc,wx)
export WINEPREFIX=${UWPH_FIXTURE_PREFIX:-$PWD/out/wine-o4/prefix}
mkdir -p out/wine-o4
exec 9>out/wine-o4/lock
flock -w 120 9 || { echo 'Owned Wine fixture already in use' >&2; exit 1; }
[[ -f $WINEPREFIX/system.reg ]] || { echo "Missing initialized fixture: $WINEPREFIX" >&2; exit 1; }
# Reports are written from inside the jail: /tmp is writable under appfs and
# EXTRA_RW is bound under noproc; the repo root (INST) stays read-only.
work=$(mktemp -d /tmp/uwph-battery.XXXXXXXX)
mkdir -p out/battery
logs=$(mktemp -d "$PWD/out/battery/run.XXXXXXXX")
trap 'rm -rf -- "$work"' EXIT
export EXTRA_RW=$work WINEDEBUG=-all WINEDLLOVERRIDES=wineboot.exe=d
export WINE_LOADER=/usr/lib/wine/wine64
# Retained concession: an explicit stock Linux server outside the jail.
/usr/lib/wine/wineserver64 -p 180 9>&- || true
export WINESERVER=/bin/false
wrapper=${UWPH_UWP_RUN:-/workspace/xbox-readiness/tests/uwp_run.sh}
fails=0
for c in "${sets[@]}"; do
    for p in "${probes[@]}"; do
        stem=${c//,/+}-$p
        rc=0
        timeout --kill-after=2s 30s "$wrapper" "$c" "$root" -- env \
            LD_PRELOAD="$PWD/out/libuwph_wine_shim.so" UWPH_REPORT="$work/$stem.report" \
            "$PWD/out/run_via_wine" "$bin/$p.exe" \
            > "$logs/$stem.log" 2>&1 9>&- || rc=$?
        cp "$work/$stem.report" "$logs/" 2>/dev/null || :
        # Sum of uwph trace counters in the launcher's report: >0 proves uwph ran.
        traced=0
        if [[ -f $logs/$stem.report ]]; then
            traced=$(awk '$1=="trace" { n+=$3 } END { print n+0 }' "$logs/$stem.report")
        fi
        result=PASS
        if [[ $rc != 0 || $traced == 0 ]] || grep -q '^SUMMARY .* fail=[1-9]' "$logs/$stem.log"; then
            result=FAIL; fails=$((fails + 1))
        fi
        printf '%s\t%s\t%s\t%s\n' "$c" "$p" "$rc" "$traced"
        printf '%s %s %s (log: %s)\n' "$result" "$c" "$p" "$logs/$stem.log" >&2
    done
done
total=$(( ${#sets[@]} * ${#probes[@]} ))
echo "battery: $((total - fails))/$total PASS THROUGH launcher -> Wine loader -> shim -> uwph; logs $logs; Linux proof, separate stock wineserver, Xbox untested" >&2
(( fails == 0 ))
