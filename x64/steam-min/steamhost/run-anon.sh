#!/usr/bin/env bash
# B78 O2: isolated credential-free stock Wine route. No seed-login invocation.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PIN="$HERE/PIN.txt" EVID= WAIT=360 SERVE=0 PROBE=
while (($#)); do
    case $1 in
        --pin) PIN=$(realpath "$2"); shift ;;
        --anon|--stock) ;;
        --serve) SERVE=$2; shift ;;
        --probe) PROBE=$2; shift ;;
        --wait) WAIT=$2; shift ;;
        --evidence) EVID=$2; shift ;;
        *) echo "unsupported anonymous argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[[ -n $EVID && $WAIT =~ ^[0-9]+$ ]] && ((WAIT > 0 && WAIT <= 3600)) || exit 2
[[ $(sed -n 's/^manifest_version=//p' "$PIN") == 1745623383 ]] || exit 2
[[ $SERVE =~ ^[0-9]+$ ]] && ((SERVE <= 3600)) || exit 2
[[ -z $PROBE || ( $PROBE == two-process && $SERVE -gt 0 ) ]] || exit 2
umask 077
mkdir -p "$EVID" "$HERE/out" /workspace/x64-run/videos
EVID=$(realpath "$EVID")
# Reverify the cached, SHA2-checked Valve archive; fetcher enforces free space on cache miss.
bash "$HERE/fetch-pin.sh" --pin "$PIN" >"$EVID/fetch.log"
make -C "$HERE" PIN_FILE="$PIN" OUT=out/build-1745623383
PFX=$(mktemp -d "$HERE/out/anon-pfx.XXXXXXXX")
export WINEPREFIX=$PFX WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml='
XVFB= REC= SAMPLER= TERM_PID= HOST=
cleanup() {
    for pid in "$HOST" "$SAMPLER" "$TERM_PID"; do [[ -z $pid ]] || kill "$pid" 2>/dev/null || true; done
    [[ -z $REC ]] || { kill -INT "$REC" 2>/dev/null || true; wait "$REC" 2>/dev/null || true; }
    wineserver -k 2>/dev/null || true
    timeout 10 wineserver -w 2>/dev/null || true
    [[ -z $XVFB ]] || kill "$XVFB" 2>/dev/null || true
    rm -rf "$PFX"
    echo prefix_deleted=1 >"$EVID/cleanup.log"
}
trap cleanup EXIT
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$PFX/display" >"$EVID/xvfb.log" 2>&1 &
XVFB=$!
for _ in {1..50}; do [[ ! -s $PFX/display ]] || break; sleep .1; done
export DISPLAY=:$(cat "$PFX/display")
timeout 60 wineboot -i >"$EVID/wineboot.log" 2>&1
wineserver -w
bash "$HERE/../net/fix-prefix-tls.sh" --prefix "$PFX" --evidence "$EVID/tls" >"$EVID/tls-setup.log" 2>&1
STEAM="$PFX/drive_c/Program Files (x86)/Steam"
mkdir -p "$STEAM"
cp "$HERE/out/client-1745623383/"{steamclient64,tier0_s64,vstdlib_s64}.dll "$STEAM/"
cp "$HERE/out/build-1745623383/steamhost64.exe" "$STEAM/"
! test -e "$STEAM/config/loginusers.vdf"
echo fresh_prefix_no_loginusers=1 >"$EVID/prefix-check.log"
printf 'route=stock Wine, anonymous, build 1745623383\nNo uwph or console proof.\n' >"$EVID/route.txt"
: >"$EVID/steamhost.raw"
xterm -geometry 110x30 -e tail -F "$EVID/steamhost.raw" >"$EVID/xterm.log" 2>&1 &
TERM_PID=$!
VIDEO=/workspace/x64-run/videos/B78-O2.mp4
[[ -z $PROBE ]] || VIDEO=/workspace/x64-run/videos/B79-O1.mp4
ffmpeg -nostdin -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" -t 55 -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$VIDEO" >"$EVID/video.log" 2>&1 &
REC=$!
(while :; do ps -eo comm= | grep -ci '^steamwebhelper' || :; sleep 1; done) >"$EVID/webhelper_samples" &
SAMPLER=$!
# Build our probe and copy only the game's original API DLL beside it.
if [[ -n $PROBE ]]; then
    mkdir -p "$PFX/probe"
    x86_64-w64-mingw32-gcc -O2 -Wall -o "$PFX/probe/steamapi_probe.exe" "$HERE/../steamapi_probe.c"
    API=/workspace/steam-owned/games/megabonk/Megabonk_Data/Plugins/x86_64/steam_api64.dll
    cp "$API" "$PFX/probe/steam_api64.dll"
    cmp "$API" "$PFX/probe/steam_api64.dll"
    sha256sum "$API" >"$EVID/steam-api-sha256.txt"
    printf '3405340\n' >"$PFX/probe/steam_appid.txt"
    echo 'probe_appid=3405340 (Megabonk)' >>"$EVID/route.txt"
    : >"$EVID/probe.log"
fi
ARGS=(--anon --wait "$WAIT")
((SERVE == 0)) || ARGS+=(--serve "$SERVE")
set +e
(cd "$STEAM" && exec timeout $((WAIT + SERVE + 60)) wine ./steamhost64.exe "${ARGS[@]}") >"$EVID/steamhost.raw" 2>"$EVID/wine.log" &
HOST=$!
if [[ -n $PROBE ]]; then
    for ((i=0; i<WAIT+60; i++)); do
        grep -q '^serving=' "$EVID/steamhost.raw" && break
        kill -0 "$HOST" 2>/dev/null || break
        sleep 1
    done
    if grep -q '^serving=' "$EVID/steamhost.raw"; then
        (cd "$PFX/probe" && timeout 45 wine ./steamapi_probe.exe steam_api64.dll) 2>"$EVID/probe-wine.log" | tr -d '\r' >"$EVID/probe.log"
        PROBE_RC=${PIPESTATUS[0]}
    else
        echo 'probe_skipped=host did not reach serving state' >"$EVID/probe.log"
        PROBE_RC=11
    fi
fi
wait "$HOST"
RC=$?
HOST=
[[ -z $PROBE || $RC != 0 ]] || RC=$PROBE_RC
set -e
tr -d '\r' <"$EVID/steamhost.raw" >"$EVID/steamhost.log"
rm "$EVID/steamhost.raw"
kill "$SAMPLER"; wait "$SAMPLER" 2>/dev/null || true; SAMPLER=
COUNT=$(sort -n "$EVID/webhelper_samples" | tail -1)
[[ -n $COUNT ]] || COUNT=nosamples
[[ -z $(find "$PFX" \( -iname 'steamwebhelper*' -o -iname 'libcef*' \) -print -quit) ]] || COUNT=cef-present
printf 'webhelper_count=%s\n' "$COUNT" >>"$EVID/steamhost.log"
# Only anonymous IDs can exist; redact numeric SteamIDs nonetheless.
if [[ -f $STEAM/logs/connection_log.txt ]]; then
    sed -E 's/[0-9]{17}/<steamid>/g' "$STEAM/logs/connection_log.txt" >"$EVID/connection_log.txt"
fi
! test -e "$STEAM/config/loginusers.vdf"
printf '%s\n' "$RC" >"$EVID/exit-code"
cat "$EVID/steamhost.log"
exit "$RC"
