#!/usr/bin/env bash
# No credentials: exercise QR begin + polling THROUGH the constrained launcher.
# Intentionally leaves confirmation to the owner, then times out. This proves
# session creation/polling, not successful mobile confirmation or console login.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
out=$root/x64/launcher-dl/out
h=$root/x64/uwp-host/harness
[[ -f $out/steamdl.exe && -d $h/prebuilt-prefix ]]
umask 077
run=$(mktemp -d "$out/qr-XXXXXXXX")
cp -a --reflink=auto "$h/prebuilt-prefix" "$run/prefix"
chmod -R u+w "$run/prefix"
export WINEPREFIX=$run/prefix EXTRA_RW=$out UWPH_INPROC_SERVER=1 WINESERVER=/bin/false
unset UWPH_INPROC_OWNER WINEDLLOVERRIDES
rc=0
timeout --kill-after=5s 25 /workspace/xbox-readiness/tests/uwp_run.sh nofork,wx,noproc "$root" -- env \
  LD_PRELOAD="$h/out/libuwph_wine_shim.so" UWPH_REPORT="$run/report" UWPH_REPORT_PID=1 \
  WINE_LOADER="$h/out/wine-wx/lib/wine/wine64" WINEDEBUG=-all \
  "$h/out/run_via_wine" "$out/steamdl.exe" "Z:${run//\//\\}\\content" --qr > "$run/qr.log" 2>&1 || rc=$?
cat "$run/qr.log"
[[ $rc == 124 ]]
grep -q '^https://s.team/q/' "$run/qr.log"
grep -q '^QR poll acknowledged' "$run/qr.log"
! grep -q '^FAIL ' "$run/qr.log"
echo 'PASS QR session creation/polling THROUGH launcher (unconfirmed; intentionally timed out; Xbox untested)'
