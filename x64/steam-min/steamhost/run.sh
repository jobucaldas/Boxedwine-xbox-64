#!/bin/bash
# O1 only: isolated stock-Wine loader smoke test, no credentials or logon.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
EVID= MODE= SELFTEST=
while (($#)); do
    case $1 in
        --stock) MODE=stock ;;
        --selftest) SELFTEST=1 ;;
        --evidence) EVID=$2; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
[[ $MODE == stock && $SELFTEST == 1 && -n $EVID ]] || exit 2
umask 077
mkdir -p "$EVID" "$HERE/out" /workspace/x64-run/videos
EVID=$(cd "$EVID" && pwd)
# Fetch through the existing manifest/SHA2 verifier only if not already staged.
if [[ ! -f $HERE/out/client/steamclient64.dll ]]; then
    FREE=$(df --output=avail -B1 / | tail -1)
    ((FREE >= 10*1024*1024*1024)) || { echo 'need >=10GiB free before download' >&2; exit 1; }
    python3 "$HERE/../fetch-valve-client.py" --out "$HERE/out/client" >"$EVID/fetch.log"
fi
make -C "$HERE"
PFX=$(mktemp -d "$HERE/out/pfx.XXXXXXXX")
export WINEPREFIX=$PFX WINEARCH=win64 WINEDEBUG=-all
export WINEDLLOVERRIDES='mscoree,mshtml='
XVFB= REC=
cleanup() {
    [[ -z $REC ]] || { kill -INT "$REC" 2>/dev/null || true; wait "$REC" 2>/dev/null || true; }
    wineserver -k 2>/dev/null || true
    wineserver -w 2>/dev/null || true
    [[ -z $XVFB ]] || kill "$XVFB" 2>/dev/null || true
    rm -rf "$PFX"
}
trap cleanup EXIT
# -displayfd asks Xvfb itself to allocate a free display.
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$PFX/display" >"$PFX/xvfb.log" 2>&1 &
XVFB=$!
for _ in {1..50}; do [[ ! -s $PFX/display ]] || break; sleep .1; done
export DISPLAY=:$(cat "$PFX/display")
timeout 60 wineboot -i >"$EVID/wineboot.log" 2>&1
wineserver -w
STEAM="$PFX/drive_c/steamhost"
mkdir -p "$STEAM"
cp -a "$HERE/out/client/." "$STEAM/"
cp "$HERE/out/steamhost64.exe" "$STEAM/"
printf 'route=stock Wine\nscope=O1 build-only plus loader selftest; no uwph or console proof\n' >"$EVID/route.txt"
# Show the credential-free selftest log in the requested short Xvfb video.
xterm -geometry 110x30 -e bash -c 'tail -F "$1"' _ "$EVID/steamhost.log" >/dev/null 2>&1 &
ffmpeg -nostdin -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" -t 55 -c:v libx264 -preset ultrafast -pix_fmt yuv420p /workspace/x64-run/videos/B72-O1.mp4 >"$EVID/video.log" 2>&1 &
REC=$!
sleep 1
set +e
timeout 45 wine "$STEAM/steamhost64.exe" --selftest >"$EVID/steamhost.log" 2>"$EVID/wine.log"
RC=$?
set -e
cat "$EVID/steamhost.log"
printf '%s\n' "$RC" >"$EVID/exit-code"
sleep 2
exit "$RC"
