#!/bin/bash
# B50 O2: Valve's REAL Steam client (steam.exe + steamclient64.dll, unmodified, no CEF
# installed) on STOCK Wine, logged on from Steam's own login cache (seed-login.py), then
# our steamapi_probe.exe and Megabonk (unmodified, symlink farm) talk to it through the
# game's own unmodified steam_api64.dll.
#
# Usage: run-minimal.sh --stock --evidence DIR [--steam-wait 120] [--game-secs 60] [--video FILE]
#   --uwph is O3 (not implemented here).
# Evidence: probe.log, Player.log, webhelper_samples, webhelper_count, processes.txt,
#   flags.txt, logon.txt, steam-wine.log / game-wine.log (+process), steam-logs/ (redacted).
# Linux stock-Wine proof only; nothing here runs THROUGH uwph.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MODE= EVID= STEAM_WAIT=120 GAME_SECS=60 VIDEO=/workspace/x64-run/videos/B50-O2.mp4
while [ $# -gt 0 ]; do
    case $1 in
        --stock) MODE=stock ;;
        --uwph) echo "run-minimal.sh: --uwph is B50 O3, not implemented" >&2; exit 3 ;;
        --evidence) EVID=$2; shift ;;
        --steam-wait) STEAM_WAIT=$2; shift ;;
        --game-secs) GAME_SECS=$2; shift ;;
        --video) VIDEO=$2; shift ;;
        *) echo "run-minimal.sh: unknown arg $1" >&2; exit 2 ;;
    esac
    shift
done
[ "$MODE" = stock ] && [ -n "$EVID" ] || { echo "usage: $0 --stock --evidence DIR" >&2; exit 2; }

OUT=$HERE/out
PFX=$OUT/pfx
STEAM="$PFX/drive_c/Program Files (x86)/Steam"
GAME_SRC=/workspace/steam-owned/games/megabonk
FARM=$OUT/megabonk-farm
APPID=3405340
SETS=bins_win64,steam_win64,bins_codecs_win64,bins_misc_win64,public_all,strings_en_all,resources_all
export WINEPREFIX=$PFX WINEARCH=win64

umask 077
mkdir -p "$EVID" "$(dirname "$VIDEO")"
EVID=$(cd "$EVID" && pwd)
rm -rf "$EVID/steam-logs"
rm -f "$EVID"/{probe.log,Player.log,webhelper_samples,webhelper_samples_raw,webhelper_count,processes.txt,flags.txt,logon.txt,steam-wine.log,game-wine.log,probe-wine.log,run.log,seed.log}
exec > >(tee "$EVID/run.log") 2>&1
log() { echo "[run-minimal $(date +%T)] $*"; }

# --- 1. Valve's client tree, from Valve's manifest only (cached, sha2-verified) -------
python3 "$HERE/fetch-valve-client.py" --set "$SETS" --out "$STEAM" | tail -1 || exit 1
if [ -n "$(find "$PFX" -iname 'steamwebhelper*' -o -iname 'libcef*' | head -1)" ]; then
    log "FATAL: CEF/webhelper file present in prefix"; exit 1
fi
# Valve's documented steam.cfg: no bootstrapper self-update (it would fetch CEF back).
printf 'BootStrapperInhibitAll=enable\nBootStrapperForceSelfUpdate=disable\n' > "$STEAM/steam.cfg"

# --- 2. our two helpers (mingw x86_64) --------------------------------------------------
mkdir -p "$OUT/bin"
x86_64-w64-mingw32-gcc -O2 -Wall -o "$OUT/bin/steamapi_probe.exe" "$HERE/steamapi_probe.c" || exit 1
x86_64-w64-mingw32-gcc -O2 -Wall -o "$OUT/bin/dpapi_protect.exe" "$HERE/dpapi_protect.c" -lcrypt32 || exit 1

# --- 3. private Xvfb ----------------------------------------------------------------------
for d in $(seq 170 239); do
    [ -e /tmp/.X11-unix/X$d ] || [ -e /tmp/.X$d-lock ] || { DNUM=$d; break; }
done
Xvfb :$DNUM -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 &
XVFB=$!
export DISPLAY=:$DNUM
sleep 1
PIDS=()
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    wineserver -k 2>/dev/null
    kill $XVFB 2>/dev/null
}
trap cleanup EXIT
log "Xvfb $DISPLAY"

# --- 4. fresh prefix = O1 tree (Valve files kept, Wine + Steam state reset) ---------------
wineserver -k 2>/dev/null
rm -rf "$PFX"/*.reg "$PFX"/.update-timestamp "$PFX"/dosdevices "$PFX/drive_c/users" \
       "$PFX/drive_c/windows" "$PFX/drive_c/ProgramData" "$PFX/drive_c/Program Files" \
       "$STEAM"/{config,logs,userdata,appcache,depotcache,steamapps,dumps,htmlcache}
mkdir -p "$PFX"
WINEDEBUG=-all WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i >/dev/null 2>&1
wineserver -w
[ -f "$PFX/system.reg" ] || { log "FATAL: wineboot failed"; exit 1; }
log "prefix initialised"

# --- 5. Steam's own login state, seeded from the SteamClient refresh token ----------------
python3 "$HERE/seed-login.py" --prefix "$PFX" --helper "$OUT/bin/dpapi_protect.exe" | tee "$EVID/seed.log"
grep -qx 'seeded=1' "$EVID/seed.log" || { log "FATAL: seed-login failed"; exit 1; }

# --- 6. webhelper sampler -------------------------------------------------------------
# Keep the exact required global process count, including any ambient matches.
( while :; do pgrep -fc -i steamwebhelper || :; sleep 1; done ) > "$EVID/webhelper_samples" 2>/dev/null &
SAMPLER=$!
PIDS+=($SAMPLER)
SAMPLER_RAW=$SAMPLER

# optional video: an xterm with the live evidence beside the game window
if [ -n "$VIDEO" ]; then
    xterm -geometry 100x16+0+470 -fa Mono -fs 9 -T evidence -e \
        "tail -n +1 -F '$EVID/logon.txt' '$EVID/probe.log' '$EVID/webhelper_samples' 2>/dev/null" &
    PIDS+=($!)
fi

# --- 7. Valve's steam.exe, minimal flags, +process ---------------------------------------
FLAGS="-silent -nofriendsui -nochatui -nobigpicture -nointro -noverifyfiles"
log "start steam.exe $FLAGS"
( cd "$STEAM" && WINEDEBUG=+process exec wine steam.exe $FLAGS ) > "$EVID/steam-wine.log" 2>&1 &
STEAM_PID=$!
PIDS+=($STEAM_PID)

CONN="$STEAM/logs/connection_log.txt"
LOGON=0
for i in $(seq 1 "$STEAM_WAIT"); do
    if [ -f "$CONN" ] && grep -aqE "LogOnResponse.*'OK'|\[Logged On\]" "$CONN"; then LOGON=1; break; fi
    if [ -f "$CONN" ] && grep -aqE "LogOnResponse.*'(Invalid|Access|Expired|Revoked|Denied)" "$CONN"; then break; fi
    sleep 1
done
log "logon=$LOGON after ${i}s"

if [ "$LOGON" = 1 ]; then
# --- 8. probe: game's unmodified steam_api64.dll -> Valve's steamclient64 -----------------
API_W=$(WINEDEBUG=-all winepath -w "$GAME_SRC/Megabonk_Data/Plugins/x86_64/steam_api64.dll")
WINEDEBUG=+process timeout 60 wine "$OUT/bin/steamapi_probe.exe" "$API_W" \
    2> "$EVID/probe-wine.log" | tr -d '\r' > "$EVID/probe.log"
log "probe:"; cat "$EVID/probe.log"

# --- 9. Megabonk from a symlink farm (install untouched), -force-d3d11, GAME_SECS --------
rm -rf "$FARM"
cp -as "$GAME_SRC" "$FARM"
PLAYER_W=$(WINEDEBUG=-all winepath -w "$EVID/Player.log")
if [ -n "$VIDEO" ]; then
    ffmpeg -y -loglevel error -f x11grab -video_size 1280x720 -framerate 15 -i "$DISPLAY" \
        -t 58 -c:v libx264 -preset veryfast -pix_fmt yuv420p "$VIDEO" &
    FFMPEG=$!
fi
log "start Megabonk.exe -force-d3d11 (${GAME_SECS}s)"
( cd "$FARM" && SteamAppId=$APPID SteamGameId=$APPID WINEDEBUG=+process \
    exec timeout -s KILL "$GAME_SECS" wine Megabonk.exe -force-d3d11 -screen-fullscreen 0 \
    -screen-width 1280 -screen-height 450 -logFile "$PLAYER_W" ) > "$EVID/game-wine.log" 2>&1
log "Megabonk ended rc=$?"
[ -n "$VIDEO" ] && wait $FFMPEG 2>/dev/null

else
    log "BLOCKED: Valve client did not reach logged-on state; no alternate credential path attempted"
    printf "BLOCKED: no logged-on state after %ss\nProbe and game skipped; see Steam logs.\n" "$STEAM_WAIT" > "$EVID/logon.txt"
    if [ -n "$VIDEO" ]; then
        mkdir -p "$(dirname "$VIDEO")"
        ffmpeg -y -loglevel error -f x11grab -video_size 1280x720 -framerate 15 -i "$DISPLAY" \
            -t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p "$VIDEO"
    fi
fi
kill "$SAMPLER" 2>/dev/null; wait "$SAMPLER" 2>/dev/null

# --- 10. shut Steam down, collect evidence ------------------------------------------------
( cd "$STEAM" && WINEDEBUG=-all timeout 30 wine steam.exe -shutdown ) >/dev/null 2>&1
sleep 5
wineserver -k 2>/dev/null

# redact account name / SteamID / account id from copied Steam logs
SID=$(sed -n 's/^\t"\([0-9]\{17\}\)"$/\1/p' "$STEAM/config/loginusers.vdf" | head -1)
ACC=$(sed -n 's/^\t\t"AccountName"\t\t"\(.*\)"$/\1/p' "$STEAM/config/loginusers.vdf" | head -1)
AID=$(( ${SID:-76561197960265728} - 76561197960265728 ))
redact() { sed -e "s/$SID/<steamid>/g" -e "s/\[U:1:$AID\]/[U:1:<id>]/g" -e "s/$AID/<id>/g" -e "s/$ACC/<account>/gI"; }
mkdir -p "$EVID/steam-logs"
for f in "$STEAM"/logs/*.txt; do [ -f "$f" ] && redact < "$f" > "$EVID/steam-logs/$(basename "$f")"; done
for f in steam-wine.log game-wine.log probe-wine.log; do
    [ -f "$EVID/$f" ] || continue
    redact < "$EVID/$f" > "$EVID/$f.tmp" && mv "$EVID/$f.tmp" "$EVID/$f"
done
[ -f "$EVID/Player.log" ] && { redact < "$EVID/Player.log" > "$EVID/Player.tmp"; mv "$EVID/Player.tmp" "$EVID/Player.log"; }

grep -aE "LogOnResponse|Logged On|Logging on|Log[Oo]n|logon" "$EVID/steam-logs/connection_log.txt" 2>/dev/null > "$EVID/logon.txt"
echo "logon=$LOGON" >> "$EVID/logon.txt"

# processes started (Wine +process: CreateProcessInternalW app/cmdline)
grep -ahoE 'CreateProcessInternalW app L"[^"]*"' "$EVID/steam-wine.log" | sed 's/.*app L"//; s/"$//' \
    | sort | uniq -c > "$EVID/processes.txt"
{ echo "--- cmdlines (steam.exe process tree)"; grep -ahoE 'CreateProcessInternalW app .*cmdline .*' "$EVID/steam-wine.log" | sort -u
  echo "--- cmdlines (probe + Megabonk)"; grep -ahoE 'CreateProcessInternalW app .*cmdline .*' "$EVID/probe-wine.log" "$EVID/game-wine.log" | sort -u; } >> "$EVID/processes.txt"

MAXS=$(sort -n "$EVID/webhelper_samples" | tail -1)
NCP=$(cat "$EVID/steam-wine.log" "$EVID/probe-wine.log" "$EVID/game-wine.log" 2>/dev/null | grep -ai 'CreateProcessInternalW' | grep -aci 'steamwebhelper')
echo $(( ${MAXS:-0} + NCP )) > "$EVID/webhelper_count"
log "webhelper samples=$(wc -l < "$EVID/webhelper_samples") max=${MAXS:-0} createprocess=$NCP"

# which of our flags Steam's logs call unknown/unrecognised
{ echo "flags: $FLAGS"
  grep -ahiE "unknown|unrecogni[sz]ed|ignor" "$EVID"/steam-logs/*.txt | grep -aiE -- "-(silent|nofriendsui|nochatui|nobigpicture|nointro|noverifyfiles)" || echo "(no Steam log line names any of these flags as unknown/ignored)"
  echo "--- command lines Steam logged"; grep -ahiE "command ?line|cmdline" "$EVID"/steam-logs/*.txt | head -10; } > "$EVID/flags.txt"

grep -a "Steam" "$EVID/Player.log" 2>/dev/null | head -20
log "evidence in $EVID"
[ "$LOGON" = 1 ]
