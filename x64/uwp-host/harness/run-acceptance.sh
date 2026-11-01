#!/usr/bin/env bash
# O4 Linux acceptance, through our launcher and Wine's own PE loader.
set -euo pipefail
cd "$(dirname "$0")"
root=$(realpath ../../..)
make out/libuwph_wine_shim.so out/run_via_wine out/fake_proc_test
./out/fake_proc_test --host-proc
bwrap --ro-bind / / --tmpfs /proc -- ./out/fake_proc_test
export WINEPREFIX=${UWPH_FIXTURE_PREFIX:-$PWD/out/wine-o4/prefix}
[[ -f $WINEPREFIX/system.reg ]]
export EXTRA_RW=$PWD/out WINEDEBUG=-all WINEDLLOVERRIDES=wineboot.exe=d
export WINE_LOADER=/usr/lib/wine/wine64
# Retained concession: an explicit stock Linux server outside the jail.
/usr/lib/wine/wineserver64 -p 180 || true
# A server already owning this fixture returns nonzero; the guest must still pass.
export WINESERVER=/bin/false
wrapper=${UWPH_UWP_RUN:-/workspace/xbox-readiness/tests/uwp_run.sh}
constraints=${UWPH_CONSTRAINTS:-nofork,noproc,wx}
# Apply the opt-in policy inside the wrapper's namespace, before the launcher.
# Empty preserves the existing acceptance networking behavior.
net_jail=()
case ${UWPJAIL_NET:-} in
    '') ;;
    xbox) net_jail=(/workspace/xbox-readiness/bin/uwpjail --net-xbox --) ;;
    *) echo "unsupported UWPJAIL_NET: $UWPJAIL_NET" >&2; exit 2 ;;
esac
echo "acceptance networking=${UWPJAIL_NET:-legacy}"
for pe in hello_k32 hello; do
    stem=$PWD/out/accept-$pe
    rm -f "$stem.report" "$stem.log"
    rc=0
    timeout --kill-after=2s 20s "$wrapper" "$constraints" "$root" -- "${net_jail[@]}" env \
        LD_PRELOAD="$PWD/out/libuwph_wine_shim.so" UWPH_REPORT="$stem.report" \
        LD_DEBUG=bindings "$PWD/out/run_via_wine" "$root/x64/pe/tests/$pe.exe" \
        > "$stem.log" 2>&1 || rc=$?
    sed -n '/uwpjail:/p; /\[launcher\]/p; /^hello /p; /^hello.exe:/p; /^TEB self-check:/p; /^malloc:/p; /^threads:/p' "$stem.log"
    echo "$pe constraints=$constraints exit=$rc"
    [[ $rc == 0 ]]
    if [[ $pe == hello ]]; then
        grep -q 'TEB self-check: ok' "$stem.log"
        grep -q 'threads: shared=200000' "$stem.log"
    else
        grep -q '^hello ' "$stem.log"
    fi
    python3 verify_noproc.py "$stem.report"
    python3 verify_acceptance.py "$stem.report" "$stem.log"
done
echo "PASS O4 both hellos THROUGH launcher -> Wine loader -> shim -> uwph ($constraints); Linux proof, separate stock wineserver, Xbox untested"
