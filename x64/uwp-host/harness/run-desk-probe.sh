#!/usr/bin/env bash
# Desktop oracle: stock baseline or the full Megabonk nofork stack.
# Linux proof only; Xbox untested. Requires a private Xvfb DISPLAY.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
here=$PWD
mode=${1:-}
case "$mode" in stock|stock-selfhost|nofork) ;; *) echo "usage: $0 stock|stock-selfhost|nofork" >&2; exit 2 ;; esac
: "${DISPLAY:?run-desk-probe.sh needs DISPLAY (private Xvfb)}"
probe=$(realpath ../../pe/tests/desk_probe.exe)
[[ -f $probe ]]
make -s prebuilt-prefix
if [[ $mode == stock-selfhost ]]; then
    make -s out/run_via_wine out/wine-wx/.desk-stamp
fi
if [[ $mode == nofork ]]; then
    make -s out/libuwph_wine_shim.so out/run_via_wine out/wine-wx/.stamp
fi
out=$here/out
mkdir -p "$out/desk-probe"
source ./prefix-hygiene.sh
refuse_dereferenced_prefix "$here/prebuilt-prefix"
run=$(mktemp -d "$out/desk-probe/run-XXXXXXXX")
cp -a --reflink=auto "$here/prebuilt-prefix" "$run/prefix"
chmod -R u+w "$run/prefix"
export WINEPREFIX=$run/prefix
unset WINEDLLOVERRIDES UWPH_INPROC_OWNER UWPH_INPROC_SERVER WINESERVER LD_PRELOAD
log=$run/wine.log
rc=0
echo "DESKTOP PROBE mode=$mode DISPLAY=$DISPLAY"
if [[ $mode == stock || $mode == stock-selfhost ]]; then
    args=()
    loader=wine
    if [[ $mode == stock-selfhost ]]; then
        export WINE_LOADER="$out/wine-wx/lib/wine/wine64"
        loader=$out/run_via_wine
        export WINEDLLPATH="$out/wine-wx/lib/wine"
        cp "$out/wine-wx/lib/wine/x86_64-windows/uwpdesktop.dll" "$WINEPREFIX/drive_c/windows/system32/uwpdesktop.dll"
        args=(--self-host)
    fi
    # A separate installed wineserver is allowed only for this baseline.
    /usr/lib/wine/wineserver64 -f > "$run/server.log" 2>&1 &
    server_pid=$!
    timeout --kill-after=5s "${K_TIMEOUT:-25}s" env WINEDEBUG=+process,err "$loader" "$probe" "${args[@]}" > "$log" 2>&1 || rc=$?
else
    export EXTRA_RW=$out UWPH_INPROC_SERVER=1 WINESERVER=/bin/false
    wrapper=${UWPH_UWP_RUN:-/workspace/xbox-readiness/tests/uwp_run.sh}
    timeout --kill-after=5s "${K_TIMEOUT:-25}s" "$wrapper" nofork,wx,noproc "$(dirname "$probe")" -- env \
        LD_PRELOAD="$out/libuwph_wine_shim.so" UWPH_REPORT="$run/report" UWPH_REPORT_PID=1 \
        WINE_LOADER="$out/wine-wx/lib/wine/wine64" WINEDEBUG=+process,err \
        "$out/run_via_wine" "$probe" > "$log" 2>&1 || rc=$?
fi
# Wine emits CRLF for the PE console output; normalize the displayed oracle.
sed 's/\r$//' "$log" | grep '^DESK ' || true
echo "exit=$rc"
python3 - "$log" <<'PYSPAWN'
import re
import sys

quoted = r'L"(?:\\.|[^"\\])*"'
pattern = re.compile(r':trace:process:NtCreateUserProcess .*? image (' + quoted
                     + r') cmdline (' + quoted + r') parent ')
attempts = []
with open(sys.argv[1], encoding="utf-8", errors="replace") as stream:
    for line in stream:
        match = pattern.search(line)
        if match:
            attempts.append(match.groups())
internal = sum(image[2:-1].replace("\\\\", "\\").lower().startswith("c:\\windows\\")
               for image, _ in attempts)
print(f"NtCreateUserProcess attempts: {len(attempts)} (wine-internal: {internal}, guest: {len(attempts) - internal})")
for image, cmdline in attempts:
    print(f"spawn: {image} {cmdline}")
PYSPAWN
grep -m5 'err:\|Operation not permitted' "$log" || true
if [[ $mode == nofork ]]; then
    echo "THROUGH uwp_run.sh(nofork,wx,noproc) -> launcher -> Wine own W^X loader -> shim -> uwph; Wine server hosted in-process"
else
    echo "stock Wine mode=$mode; separate wineserver allowed; Wine own PE loader (not THROUGH uwph)"
fi
echo "evidence: ${run%/run-*}/evidence-${run##*/run-}"
exit "$rc"
