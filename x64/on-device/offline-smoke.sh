#!/usr/bin/env bash
# Off-device smoke on this Linux box. Not Xbox, UWP activation, or console proof.
set -euo pipefail
cd -- "$(dirname -- "$0")"
command -v cc >/dev/null
test -x /usr/lib/wine/wine64

echo '== 1/3 package (loose payload + test cert; build-only) =='
make --no-print-directory package

echo '== 2/3 offline ladder (stage a THROUGH public uwph Linux backend) =='
./out/ladder --offline

echo '== 3/3 payload smoke.exe THROUGH our run_via_wine -> Wine loader =='
cc -O2 -Wall -Wextra -Werror -o out/run_via_wine ../pe/launcher/run_via_wine.c
# Disposable prefix: never ~/.wine or a shared fixture. Desktop Wine only.
prefix=$(mktemp -d /tmp/r4d-smoke-pfx.XXXXXX)
cleanup() {
    WINEPREFIX=$prefix /usr/lib/wine/wineserver -k >/dev/null 2>&1 || true
    rm -rf -- "$prefix"
}
trap cleanup EXIT
log=out/offline-smoke.wine.log
WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' \
    WINE_LOADER=/usr/lib/wine/wine64 \
    timeout --kill-after=2s 90s ./out/run_via_wine out/payload/smoke.exe \
    > "$log" 2> out/offline-smoke.wine.err
grep '^\[launcher\] exec /usr/lib/wine/wine64 ' out/offline-smoke.wine.err
cat "$log"
grep -q '^Boxedwine on-device skeleton: x64 Windows build OK' "$log"

echo '== artifacts =='
printf 'Loose payload: %s/out/payload/AppxManifest.xml\n' "$PWD"
printf 'Test certificate: %s/out/test-cert.cer\n' "$PWD"
printf 'Install steps: %s/README.md\n' "$PWD"
echo 'OFFLINE SMOKE PASS (Linux host only; no Xbox, UWP activation, or console proof)'
