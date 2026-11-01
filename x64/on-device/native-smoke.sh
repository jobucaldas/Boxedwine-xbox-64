#!/usr/bin/env bash
# Desktop stand-in only: public uwph backend + our launcher + Wine's own loader.
set -euo pipefail
cd -- "$(dirname -- "$0")"
make ladder package
./out/ladder --offline
cc -O2 -Wall -Wextra -Werror -o out/run_via_wine ../pe/launcher/run_via_wine.c
prefix=$(mktemp -d /tmp/r4d-o5-pfx.XXXXXX)
cleanup() {
    WINEPREFIX=$prefix /usr/lib/wine/wineserver -k >/dev/null 2>&1 || true
    rm -rf -- "$prefix"
}
trap cleanup EXIT
mkdir -p out/native-localstate
WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' \
    WINE_LOADER=/usr/lib/wine/wine64 timeout --kill-after=2s 45s \
    ./out/run_via_wine out/payload/rungs.exe --console \
    --local-state "Z:$(realpath out/native-localstate)" \
    >out/native-smoke.log 2>out/native-smoke.err
cat out/native-smoke.err out/native-smoke.log
count=$(grep -cE '^STAGE n[1-7] ' out/native-smoke.log)
test "$count" -eq 7
! grep -qE '^STAGE n[1-7] [^ ]+ PASS:' out/native-smoke.log
echo 'O5 desktop collection complete; no Xbox PASS claimed.'
