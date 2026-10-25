#!/usr/bin/env bash
# O2: Linux proof THROUGH our launcher and Wine's own loader/uwph.
set -euo pipefail
cd "$(dirname "$0")/../.."
root=$PWD
out=$root/x64/launcher-dl/out
h=$root/x64/uwp-host/harness
umask 077
mkdir -p "$out" "$root/x64/steam-min/out"
exec 9>"$out/owned.lock"
flock -n 9
private=$(mktemp -d "$root/x64/steam-min/out/owned-XXXXXXXX")
auth=$out/owned-auth.private
cleanup() { rm -f "$auth"; rm -rf "$private"; }
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
[[ ! -e $auth ]] || { echo 'FAIL stale private input; remove it securely first'; exit 1; }
bash x64/launcher-dl/build.sh
make -s -C "$h" prebuilt-prefix
run=$(mktemp -d "$out/owned-XXXXXXXX")
cp -a --reflink=auto "$h/prebuilt-prefix" "$run/prefix"
chmod -R u+w "$run/prefix"
# The sole authorised original-token reader stages a chmod-600 derived input.
python3 x64/steam-min/seed-login.py --launcher-token-out "$private/auth"
mv "$private/auth" "$auth"
[[ ! -e $out/megabonk-native ]] || { echo 'FAIL out/megabonk-native already exists; fresh download required'; exit 1; }
export WINEPREFIX=$run/prefix EXTRA_RW=$out UWPH_INPROC_SERVER=1 WINESERVER=/bin/false
unset UWPH_INPROC_OWNER WINEDLLOVERRIDES
printf 'PROOF THROUGH uwp_run.sh(nofork,wx,noproc) -> launcher -> Wine own loader -> uwph; Linux only\n'
rc=0
timeout --kill-after=5s 1800 /workspace/xbox-readiness/tests/uwp_run.sh nofork,wx,noproc "$root" -- env \
  LD_PRELOAD="$h/out/libuwph_wine_shim.so" UWPH_REPORT="$run/report" UWPH_REPORT_PID=1 \
  WINE_LOADER="$h/out/wine-wx/lib/wine/wine64" WINEDEBUG=-all \
  "$h/out/run_via_wine" "$out/steamdl.exe" "Z:${out//\//\\}\\megabonk-native" \
  --token-file "Z:${auth//\//\\}" > "$run/download.log" 2>&1 || rc=$?
cat "$run/download.log"
printf 'owned exit=%s\n' "$rc"
[[ $rc == 0 ]] || exit "$rc"
# A downloader success alone cannot satisfy the THROUGH proof.
python3 - "$run" <<'PY'
import pathlib, sys
p = pathlib.Path(sys.argv[1])
log = (p / 'download.log').read_text(errors='replace')
assert '[launcher]' in log and "handing guest to Wine's loader" in log
reports = list(p.glob('report.*'))
assert reports, 'missing uwph report'
assert 'seccomp wx=1 nofork=1' in log, 'missing constraint enforcement'
assert 'no separate server process' in log, 'missing in-process server'
for report in reports:
    for line in report.read_text().splitlines():
        f = line.split()
        if f[0] == 'total':
            assert int(f[3]) == 0, 'uwph bypass: ' + f[1]
        if f[0] == 'module':
            assert f[3] == f[4], 'wrapper/backend mismatch'
        if f[0] == 'classification':
            assert dict(v.split('=') for v in f[1:]) == dict(
                recursion='0', unattributed='0', module_overflow='0', instrumentation_raw_io='1')
protect = sum(int(s[2]) for r in reports for line in r.read_text().splitlines()
              if (s := line.split())[:2] == ['total', 'mprotect'])
assert protect > 0, 'no uwph protection calls'
assert 'VERIFIED app=3405340' in log, 'content not verified'
print('PASS THROUGH launcher/Wine loader/uwph; nofork,wx,noproc; Xbox untested')
PY
