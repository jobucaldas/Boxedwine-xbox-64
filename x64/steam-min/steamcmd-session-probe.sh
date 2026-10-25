#!/bin/bash
# B76 O1: token-free repro of STEAMHOST.md §2A ("steamcmd is not a client session for
# games"). Valve's steamcmd (steam_cmd_win32 manifest, sha2-verified, unmodified) in a
# small fresh prefix, steam.cfg BootStrapperInhibitAll=enable, `+login anonymous`; once
# connection_log says `[Logged On`, our steamapi_probe.exe loads the game's unmodified
# steam_api64.dll twice:
#   stage 1: registry exactly as steamcmd left it;
#   stage 2: HKCU\Software\Valve\Steam\ActiveProcess pid = steamcmd's Windows pid and
#            SteamClientDll64 = steamcmd's own steamclient64.dll (config only, as Proton's
#            steam_helper/steam.c#L65-L70 does).
# No account, no token: anonymous logon only. The prefix is deleted on exit.
#
# Usage: steamcmd-session-probe.sh --evidence DIR [--wait 420] [--tries 3] [--video FILE] [--uwph]
#   default: both x64 probes THROUGH-uwph; steamcmd uses stock Wine.
#   --uwph (default): the two probes (64-bit) go through x64/uwp-host run-wine.sh --guest (wx,noproc;
#           nofork OFF, external stock wineserver shared with steamcmd). steamcmd.exe is
#           32-bit and always runs on stock Wine.
# Evidence: summary.txt (logon=, stage1=, stage2=, ...), run.log, steamcmd.out,
#   activeprocess{1,2}.txt, probe{1,2}.log, probe{1,2}-wine.log, steam-logs/ (redacted).
# Linux harness evidence only; never Xbox device proof.
set -uo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
EVID= WAIT=420 TRIES=3 VIDEO=/workspace/x64-run/videos/B76-O1.mp4 MODE=uwph
while [ $# -gt 0 ]; do
    case $1 in
        --evidence) EVID=${2:?--evidence requires DIR}; shift ;;
        --wait) WAIT=${2:?--wait requires seconds}; shift ;;
        --tries) TRIES=${2:?--tries requires count}; shift ;;
        --video) VIDEO=${2:?--video requires FILE}; shift ;;
        --uwph) MODE=uwph ;;
        *) echo "steamcmd-session-probe.sh: unknown arg $1" >&2; exit 2 ;;
    esac
    shift
done
[ -n "$EVID" ] || { echo "usage: $0 --evidence DIR [--wait 420] [--tries 3] [--video FILE] [--uwph]" >&2; exit 2; }

[[ $WAIT =~ ^[0-9]+$ && $TRIES =~ ^[0-9]+$ ]] &&
    (( WAIT >= 1 && WAIT <= 420 && TRIES >= 1 && TRIES <= 3 )) || {
        echo '--wait must be 1..420 and --tries 1..3' >&2; exit 2;
    }

HARNESS=$(cd "$HERE/../uwp-host/harness" && pwd)
OUT=$HERE/out
PFX=$OUT/steamcmd-pfx
STEAM="$PFX/drive_c/Program Files (x86)/Steam"
API=/workspace/steam-owned/games/megabonk/Megabonk_Data/Plugins/x86_64/steam_api64.dll
export WINEPREFIX=$PFX WINEARCH=win64 WINEDEBUG=-all

umask 077
mkdir -p "$EVID"
EVID=$(cd "$EVID" && pwd)
rm -rf "$EVID/steam-logs" "$EVID/uwph-reports"
rm -f "$EVID"/{summary.txt,run.log,steamcmd.out,activeprocess1.txt,activeprocess2.txt,probe1.log,probe2.log,probe1-wine.log,probe2-wine.log,fetch.log,in}
exec > >(tee "$EVID/run.log") 2>&1
log() { echo "[steamcmd-probe $(date +%T)] $*"; }

AVAIL=$(df -BG --output=avail / | tail -1 | tr -dc 0-9)
[ "${AVAIL:-0}" -ge 10 ] || { log "FATAL: only ${AVAIL}G free on / (need >=10G)"; exit 1; }
[ -f "$API" ] || { log "FATAL: $API missing"; exit 1; }

# Setup/build stays outside any jail.
mkdir -p "$OUT/bin"
x86_64-w64-mingw32-gcc -O2 -Wall -o "$OUT/bin/steamapi_probe.exe" "$HERE/steamapi_probe.c" || exit 1
if [ "$MODE" = uwph ]; then
    make -C "$HARNESS" out/libuwph_wine_shim.so out/run_via_wine out/wine-wx/.stamp || exit 1
    mkdir -p "$OUT/b76-reports"; rm -f "$OUT/b76-reports/"*
fi
run_probe() { # $1 = tag; stdout = probe output
    local api_w
    api_w=$(winepath -w "$API")
    if [ "$MODE" = uwph ]; then
        UWPH_GUEST_RW="$OUT" UWPH_REPORT="$OUT/b76-reports/$1.report" WINEDEBUG=+process \
            timeout 90 "$HARNESS/run-wine.sh" --guest "$OUT/bin/steamapi_probe.exe" "$api_w" \
            2> "$EVID/$1-wine.log" | tr -d '\r'
    else
        WINEDEBUG=+process timeout 90 wine "$OUT/bin/steamapi_probe.exe" "$api_w" \
            2> "$EVID/$1-wine.log" | tr -d '\r'
    fi
}
# "<msg>" from the probe's "SteamAPI_Init=<0|1> msg=<errmsg>" line
stage_msg() {
    local init msg
    init=$(sed -n 's/^SteamAPI_Init=\([01]\).*/\1/p' "$1" | head -1)
    msg=$(sed -n 's/^SteamAPI_Init=[01] msg=//p' "$1" | head -1 | sed 's/  */ /g; s/ *$//')
    if [ -z "$init" ]; then echo "probe produced no SteamAPI_Init line"
    elif [ "$init" = 1 ]; then echo "SteamAPI_Init OK"
    else echo "${msg:-SteamAPI_Init failed (no message)}"; fi
}

# private Xvfb (for the evidence video only; steamcmd is a console program)
DNUM=
for d in $(seq 170 239); do
    [ -e /tmp/.X11-unix/X$d ] || [ -e /tmp/.X$d-lock ] || { DNUM=$d; break; }
done
[ -n "$DNUM" ] || { log "FATAL: no free Xvfb display"; exit 1; }
Xvfb :$DNUM -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 &
XVFB=$!
export DISPLAY=:$DNUM
PIDS=() FFMPEG=
stop_steamcmd() {
    if [ -e /proc/self/fd/9 ]; then
        ( echo quit >&9 ) 2>/dev/null   # subshell: a dead steamcmd only SIGPIPEs it
        sleep 8
        exec 9>&-
    fi
    wineserver -k 2>/dev/null; sleep 1
}
cleanup() {
    for p in "${PIDS[@]}"; do kill "$p" 2>/dev/null; done
    [ -n "$FFMPEG" ] && kill "$FFMPEG" 2>/dev/null
    wineserver -k 2>/dev/null
    kill $XVFB 2>/dev/null
    rm -f "$EVID/in"
    rm -rf "$PFX"
}
trap cleanup EXIT
log "route=$MODE (steamcmd.exe: stock Wine $(wine --version)); Xvfb $DISPLAY"

CONN="$STEAM/logs/connection_log.txt"
LOGON=0 TRY=0 SECS= PROBES_OK=0
for TRY in $(seq 1 "$TRIES"); do
    # --- fresh small prefix (no mono/gecko) + Valve's steamcmd tree ---------------------
    wineserver -k 2>/dev/null
    rm -rf "$PFX"
    mkdir -p "$PFX"
    WINEDLLOVERRIDES="mscoree,mshtml=" wineboot -i >/dev/null 2>&1
    wineserver -w
    [ -f "$PFX/system.reg" ] || { log "FATAL: wineboot failed"; exit 1; }
    python3 "$HERE/fetch-valve-client.py" --manifest steam_cmd_win32 --out "$STEAM" > "$EVID/fetch.log" 2>&1 \
        || { cat "$EVID/fetch.log"; log "FATAL: fetch failed"; exit 1; }
    tail -1 "$EVID/fetch.log"
    # Valve's documented steam.cfg: no bootstrapper self-update.
    printf 'BootStrapperInhibitAll=enable\nBootStrapperForceSelfUpdate=disable\n' > "$STEAM/steam.cfg"
    log "try $TRY: prefix $(du -sh "$PFX" | cut -f1)"

    # --- steamcmd +login anonymous, stdin held open on a fifo ------------------------------
    rm -f "$EVID/in"; mkfifo "$EVID/in"
    ( cd "$STEAM" && exec wine steamcmd.exe +@NoPromptForPassword 1 +login anonymous < "$EVID/in" ) \
        > "$EVID/steamcmd.out" 2>&1 &
    PIDS+=($!)
    exec 9> "$EVID/in"
    t0=$(date +%s)
    while [ $(( $(date +%s) - t0 )) -lt "$WAIT" ]; do
        if grep -aqF '[Logged On' "$CONN" 2>/dev/null; then LOGON=1; break; fi
        if grep -aqiE 'FAILED|Invalid Password|login failure' "$EVID/steamcmd.out"; then break; fi
        sleep 3
    done
    SECS=$(( $(date +%s) - t0 ))
    log "try $TRY: logon=$LOGON after ${SECS}s"
    [ "$LOGON" = 1 ] && break
    stop_steamcmd
done

if [ "$LOGON" = 1 ]; then
    if [ -n "$VIDEO" ]; then
        mkdir -p "$(dirname "$VIDEO")"
        xterm -geometry 160x44+0+0 -fa Mono -fs 9 -T "B76 steamcmd session probe ($MODE)" -e \
            "tail -n +1 -F '$EVID/run.log' 2>/dev/null" &
        PIDS+=($!)
        sleep 1
        ffmpeg -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" \
            -t 55 -c:v libx264 -preset veryfast -pix_fmt yuv420p "$VIDEO" &
        FFMPEG=$!
    fi
    sleep 15   # let steamcmd settle (client config) before probing
    # --- stage 1: registry as Valve left it -----------------------------------------------
    wine reg query 'HKCU\Software\Valve\Steam\ActiveProcess' 2>&1 | tr -d '\r' \
        | sed -E 's/(ActiveUser\s+REG_DWORD\s+)0x[0-9a-f]+/\1<redacted>/I' > "$EVID/activeprocess1.txt"
    grep -q 'Unable to find' "$EVID/activeprocess1.txt" && AP1=absent || AP1=present
    log "stage 1: ActiveProcess $AP1"; cat "$EVID/activeprocess1.txt"
    run_probe probe1 > "$EVID/probe1.log"; cat "$EVID/probe1.log"
    # --- stage 2: ActiveProcess pid + SteamClientDll64 -> steamcmd (config only) ----------
    WPID=$(wine tasklist 2>/dev/null | tr -d '\r' | awk 'tolower($1)=="steamcmd.exe"{print $2; exit}')
    SC=$(winepath -w "$STEAM/steamclient64.dll")
    [ -f "$STEAM/steamclient64.dll" ] || log "WARN: steamcmd tree has no steamclient64.dll"
    log "stage 2: steamcmd winpid=${WPID:-none}; SteamClientDll64=$SC"
    if [ -n "$WPID" ]; then
        wine reg add 'HKCU\Software\Valve\Steam\ActiveProcess' /v pid /t REG_DWORD /d "$WPID" /f >/dev/null 2>&1
        wine reg add 'HKCU\Software\Valve\Steam\ActiveProcess' /v SteamClientDll64 /t REG_SZ /d "$SC" /f >/dev/null 2>&1
        wine reg query 'HKCU\Software\Valve\Steam\ActiveProcess' 2>&1 | tr -d '\r' \
            | sed -E 's/(ActiveUser\s+REG_DWORD\s+)0x[0-9a-f]+/\1<redacted>/I' > "$EVID/activeprocess2.txt"
        cat "$EVID/activeprocess2.txt"
        run_probe probe2 > "$EVID/probe2.log"; cat "$EVID/probe2.log"
        STAGE2=$(stage_msg "$EVID/probe2.log")
    else
        STAGE2="not run: steamcmd.exe not in wine tasklist"
    fi
    STAGE1=$(stage_msg "$EVID/probe1.log")
    if grep -q '^SteamAPI_Init=[01] msg=' "$EVID/probe1.log" &&
       grep -q '^SteamAPI_Init=[01] msg=' "$EVID/probe2.log" &&
       grep -q '^IsSteamRunning=[01]$' "$EVID/probe2.log" &&
       [ -n "$WPID" ] && [ -f "$STEAM/steamclient64.dll" ]; then
        PROBES_OK=1
    fi
    [ -n "$FFMPEG" ] && { wait "$FFMPEG" 2>/dev/null; FFMPEG=; }
else
    log "BLOCKED: steamcmd did not reach [Logged On in $TRIES tries"
    STAGE1="not run: no logon" STAGE2="not run: no logon" AP1=unknown WPID=
fi
stop_steamcmd

# --- evidence: Steam logs, anonymous SteamIDs redacted ------------------------------------
redact() { sed -E -e 's/\[([aAgGuU]):1:[0-9]+\]/[\1:1:<id>]/g' -e 's/\b[0-9]{17}\b/<steamid>/g'; }
mkdir -p "$EVID/steam-logs"
for f in "$STEAM"/logs/*.txt; do [ -f "$f" ] && redact < "$f" > "$EVID/steam-logs/$(basename "$f")"; done
for f in steamcmd.out probe1-wine.log probe2-wine.log; do
    [ -f "$EVID/$f" ] && { redact < "$EVID/$f" > "$EVID/$f.tmp"; mv "$EVID/$f.tmp" "$EVID/$f"; }
done
[ "$MODE" = uwph ] && cp -a "$OUT/b76-reports" "$EVID/uwph-reports"
VER=$(sed -n 's/^manifest .* version \([0-9]*\)$/\1/p' "$EVID/fetch.log" | head -1)

{   echo "route=$MODE"
    echo "steamcmd_route=stock-wine"
    echo "manifest=steam_cmd_win32 version=${VER:-?}"
    echo "logon=$LOGON"
    echo "logon_try=$TRY logon_secs=${SECS:-?}"
    echo "activeprocess_stage1=$AP1"
    echo "steamcmd_winpid=${WPID:-none}"
    echo "stage1=$STAGE1"
    echo "stage2=$STAGE2"
    echo "stage2_IsSteamRunning=$(sed -n 's/^IsSteamRunning=//p' "$EVID/probe2.log" 2>/dev/null | head -1)"
} > "$EVID/summary.txt"
log "summary:"; cat "$EVID/summary.txt"

rm -rf "$PFX"
[ -d "$PFX" ] && { log "FATAL: prefix not deleted"; exit 1; }
# pattern split so this file never matches itself
if grep -rIl "ey""J" "$EVID" "$HERE"; then log "FATAL: token-like string in evidence"; exit 1; fi
log "prefix deleted; leak check OK; evidence in $EVID"
[ "$LOGON" = 1 ] && [ "$PROBES_OK" = 1 ] && grep -q '^stage2=' "$EVID/summary.txt"
