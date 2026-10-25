#!/usr/bin/env bash
# Credential-free pin gate through our uwph launcher and Wine loader.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
PIN=$(realpath "$1")
[[ -n $2 ]] || exit 2
mkdir -p "$2" "$HERE/out" /workspace/x64-run/videos
EVID=$(realpath "$2")
VERSION=$(sed -n 's/^manifest_version=//p' "$PIN")
[[ $VERSION =~ ^[0-9]+$ ]] || exit 2
CLIENT="$HERE/out/client-$VERSION"
if [[ $VERSION == 1745623383 ]]; then
    bash "$HERE/fetch-pin.sh" --pin "$PIN" >"$EVID/fetch.log"
elif [[ $PIN == "$HERE/PIN.txt" ]]; then
    if [[ ! -f $CLIENT/steamclient64.dll ]]; then
        (( $(df --output=avail -B1 / | tail -1) >= 10000000000 )) || { echo 'need >=10GB before download' >&2; exit 1; }
        python3 "$HERE/../fetch-valve-client.py" --out "$CLIENT" >"$EVID/fetch.log"
    fi
else
    echo 'unsupported pin' >&2; exit 2
fi
make -C "$HERE" PIN_FILE="$PIN" OUT="out/build-$VERSION"
HARNESS=$(cd "$HERE/../../uwp-host/harness" && pwd)
make -C "$HARNESS" out/libuwph_wine_shim.so out/run_via_wine out/wine-wx/.stamp
umask 077
PFX=$(mktemp -d "$HERE/out/check-pfx.XXXXXXXX")
export WINEPREFIX=$PFX WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml='
XVFB= REC= TERM_PID=
cleanup() {
    [[ -z $REC ]] || { kill -INT "$REC" 2>/dev/null || true; wait "$REC" 2>/dev/null || true; }
    [[ -z $TERM_PID ]] || kill "$TERM_PID" 2>/dev/null || true
    wineserver -k 2>/dev/null || true
    timeout 10 wineserver -w 2>/dev/null || true
    [[ -z $XVFB ]] || kill "$XVFB" 2>/dev/null || true
    cp "$PFX"/gate.report* "$EVID/" 2>/dev/null || true
    rm -rf "$PFX"
}
trap cleanup EXIT
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$PFX/display" >"$EVID/xvfb.log" 2>&1 &
XVFB=$!
for _ in {1..50}; do [[ ! -s $PFX/display ]] || break; sleep .1; done
export DISPLAY=:$(cat "$PFX/display")
wineserver -p 180
timeout 60 wineboot -i >"$EVID/wineboot.log" 2>&1
STEAM="$PFX/drive_c/Program Files (x86)/Steam"
mkdir -p "$STEAM"
cp "$CLIENT/"{steamclient64,tier0_s64,vstdlib_s64}.dll "$STEAM/"
cp "$HERE/out/build-$VERSION/steamhost64.exe" "$STEAM/"
! test -e "$STEAM/config/loginusers.vdf"
printf 'route=THROUGH-uwph (wx,noproc -> run_via_wine -> Wine loader -> shim)\nscope=credential-free hash/build gate only; no slot calls, no console proof; stock wineboot setup\n' >"$EVID/route.txt"
touch "$EVID/steamhost.log"
xterm -geometry 110x30 -e tail -F "$EVID/steamhost.log" >"$EVID/xterm.log" 2>&1 &
TERM_PID=$!
# Preserve the old-pin gate video when the default refusal is checked afterwards.
VIDEO=/workspace/x64-run/videos/B78-O1.mp4
[[ $VERSION == 1745623383 ]] || VIDEO="$EVID/default-gate.mp4"
ffmpeg -nostdin -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" -t 15 -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$VIDEO" >"$EVID/video.log" 2>&1 &
REC=$!
sleep 1
set +e
env UWPH_GUEST_RW="$HERE/out" UWPH_REPORT="$PFX/gate.report" timeout 45 "$HARNESS/run-wine.sh" --guest "$STEAM/steamhost64.exe" --check-pin >"$EVID/steamhost.raw" 2>"$EVID/wine.log"
RC=$?
set -e
tr -d '\r' <"$EVID/steamhost.raw" >"$EVID/steamhost.log"
rm "$EVID/steamhost.raw"
printf '%s\n' "$RC" >"$EVID/exit-code"
cat "$EVID/steamhost.log"
sleep 3
exit "$RC"
