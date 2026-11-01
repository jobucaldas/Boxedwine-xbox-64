#!/usr/bin/env bash
# Desktop evidence THROUGH our launcher and Wine's own PE loader only.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
make out/readiness-check.exe
cc -O2 -Wall -Wextra -Werror -o out/run_via_wine ../pe/launcher/run_via_wine.c
prefix=$(mktemp -d /tmp/b02-check.XXXXXX)
cleanup() {
    WINEPREFIX=$prefix /usr/lib/wine/wineserver -k >/dev/null 2>&1 || true
    rm -rf -- "$prefix"
}
trap cleanup EXIT
export WINEPREFIX=$prefix WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml=' WINE_LOADER=/usr/lib/wine/wine64
for args in 'seh' 'fs' 'net' 'thread' 'thread 1' 'thread 65' 'thread 0 1' 'thread 0 2' 'thread 0 3'; do
    # Fixed test strings only; intentional splitting produces the test arguments.
    echo "DESKTOP THROUGH-launcher check: $args"
    timeout --kill-after=2s 25s ./out/run_via_wine out/readiness-check.exe $args
done
