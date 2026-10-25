#!/usr/bin/env bash
# B78 O2: isolated credential-free stock Wine route. No seed-login invocation.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PIN="$HERE/PIN.txt" EVID= WAIT=360
while (($#)); do
    case $1 in
        --pin) PIN=$(realpath "$2"); shift ;;
        --anon|--stock) ;;
        --wait) WAIT=$2; shift ;;
        --evidence) EVID=$2; shift ;;
        *) echo "unsupported anonymous argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[[ -n $EVID && $WAIT =~ ^[0-9]+$ ]] && ((WAIT > 0 && WAIT <= 3600)) || exit 2
[[ $(sed -n 's/^manifest_version=//p' "$PIN") == 1745623383 ]] || exit 2
umask 077
mkdir -p "$EVID" "$HERE/out" /workspace/x64-run/videos
EVID=$(realpath "$EVID")
# Reverify the cached, SHA2-checked Valve archive; fetcher enforces free space on cache miss.
bash "$HERE/fetch-pin.sh" --pin "$PIN" >"$EVID/fetch.log"
make -C "$HERE" PIN_FILE="$PIN" OUT=out/build-1745623383
PFX=$(mktemp -d "$HERE/out/anon-pfx.XXXXXXXX")
export WINEPREFIX=$PFX WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml='
XVFB= REC= SAMPLER= TERM_PID=
cleanup() {
    for pid in "$SAMPLER" "$TERM_PID"; do [[ -z $pid ]] || kill "$pid" 2>/dev/null || true; done
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
ffmpeg -nostdin -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" -t 55 -c:v libx264 -preset ultrafast -pix_fmt yuv420p /workspace/x64-run/videos/B78-O2.mp4 >"$EVID/video.log" 2>&1 &
REC=$!
(while :; do ps -eo comm= | grep -ci '^steamwebhelper' || :; sleep 1; done) >"$EVID/webhelper_samples" &
SAMPLER=$!
set +e
(cd "$STEAM" && timeout $((WAIT + 60)) wine ./steamhost64.exe --anon --wait "$WAIT") >"$EVID/steamhost.raw" 2>"$EVID/wine.log"
RC=$?
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
