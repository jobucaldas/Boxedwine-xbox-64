#!/usr/bin/env bash
# B21 combo, Linux hosted proof only. No console child-process adapter.
set -euo pipefail
bundle=$(cd -- "$(dirname -- "$0")" && pwd)
repo=$(cd -- "$bundle/../../../../.." && pwd)
harness=$repo/x64/uwp-host/harness
wrapper=${UWPH_UWP_RUN:-/workspace/xbox-readiness/tests/uwp_run.sh}
work=$(mktemp -d "$harness/out/kit-combo.XXXXXX")
cleanup() {
    WINEPREFIX=$work/prefix /usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true
    rm -rf -- "$work/prefix"
}
trap cleanup EXIT
# Explicit packaging-only stock initialization; no shared or personal prefix.
cp -a --reflink=auto "$harness/prebuilt-prefix" "$work/prefix"
chmod -R u+w "$work/prefix"
export WINEPREFIX=$work/prefix WINEDEBUG=-all WINEDLLOVERRIDES=wineboot.exe=d
/usr/lib/wine/wineserver64 -p 60
export WINESERVER=/bin/false EXTRA_RW=$work
# Explicitly select the packaged Wine W^X loader; never fall back to stock.
export WINE_LOADER=$bundle/wine-wx/lib/wine/wine64
for pe in hello hello_k32; do
    stem=$work/$pe
    timeout --kill-after=2s 20s "$wrapper" nofork,noproc,wx "$repo" -- env \
        LD_PRELOAD="$bundle/libuwph_wine_shim.so" UWPH_REPORT="$stem.report" \
        LD_DEBUG=bindings "$bundle/run_via_wine" "$bundle/$pe.exe" \
        > "$stem.log" 2>&1
    grep -F "[launcher] exec $WINE_LOADER -> $bundle/$pe.exe" "$stem.log"
    if [[ $pe == hello ]]; then
        grep -q 'TEB self-check: ok' "$stem.log"
        grep -q 'threads: shared=200000' "$stem.log"
    else
        grep -q '^hello ' "$stem.log"
    fi
    python3 "$bundle/verify_noproc.py" "$stem.report"
    python3 "$bundle/verify_acceptance.py" "$stem.report" "$stem.log"
done
echo "PASS combo hello+hello_k32 nofork,noproc,wx THROUGH launcher -> bundled W^X Wine loader -> uwph; Linux only, separate stock wineserver; evidence=$work"
