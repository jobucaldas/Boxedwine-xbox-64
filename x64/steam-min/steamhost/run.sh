#!/bin/bash
# Runs of our steamhost64.exe in a throwaway prefix (deleted on exit).
#   --stock: stock Wine.  --uwph (B74): every guest (steamhost64.exe, probe, Megabonk) goes through
#   run-wine.sh --guest (uwp_run.sh wx,noproc -> our launcher -> Wine's own loader -> shim);
#   nofork OFF, two processes, stock external wineserver concession; setup (wineboot, winepath,
#   seed-login.py) stays outside the jail. Per-guest uwph reports land in EVID/uwph-reports.
#   --selftest (O1): load + CreateInterface only; no credentials, no logon.
#   default (O2):    interface-table gate, then Steam's own login cache (seed-login.py, the only
#                    token reader), then steamhost64.exe --logon; webhelper sampler.
#   --game (B73):    as default, but steamhost keeps serving after logon=1; then our steamapi_probe.exe
#                    with Megabonk's unmodified steam_api64.dll, then Megabonk (-force-d3d11, --game-secs).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
MIN=$(cd "$HERE/.." && pwd)
EVID= MODE= SELFTEST= GAME= WAIT=360 GAME_SECS=60
# Anonymous dispatch is isolated from all credential seeding paths.
for arg in "$@"; do
    if [[ $arg == --anon ]]; then exec bash "$HERE/run-anon.sh" "$@"; fi
done
PIN="$HERE/PIN.txt"
CHECK=
APPID=3405340
GAME_SRC=/workspace/steam-owned/games/megabonk
while (($#)); do
    case $1 in
        --pin) PIN=$2; shift ;;
        --check-pin) CHECK=1 ;;
        --stock) MODE=stock ;;
        --uwph) MODE=uwph ;;
        --selftest) SELFTEST=1 ;;
        --game) GAME=1 ;;
        --game-secs) GAME_SECS=$2; shift ;;
        --steam-wait) WAIT=$2; shift ;;
        --evidence) EVID=$2; shift ;;
        *) echo "unknown argument: $1" >&2; exit 2 ;;
    esac
    shift
done
if [[ $CHECK == 1 ]]; then
    exec bash "$HERE/check-pin.sh" "$PIN" "$EVID"
fi
[[ $PIN == "$HERE/PIN.txt" ]] || { echo "alternate pins require --check-pin" >&2; exit 2; }
[[ ( $MODE == stock || $MODE == uwph ) && -n $EVID && $WAIT =~ ^[0-9]+$ && $GAME_SECS =~ ^[0-9]+$ ]] || exit 2
[[ -z $GAME || -z $SELFTEST ]] || exit 2
VIDEO=/workspace/x64-run/videos/B72-O${SELFTEST:+1}${SELFTEST:-2}.mp4
[[ -z $GAME ]] || VIDEO=/workspace/x64-run/videos/B73-O1.mp4
[[ $MODE == stock ]] || VIDEO=/workspace/x64-run/videos/B74-O1.mp4
HARNESS=$(cd "$HERE/../../uwp-host/harness" && pwd)
# steamhost's Wine stderr; the uwph run keeps it apart from the other guests' logs.
HWL=wine.log
[[ $MODE == stock ]] || HWL=steamhost-wine.log
umask 077
mkdir -p "$EVID" "$HERE/out" "$MIN/out" /workspace/x64-run/videos
EVID=$(cd "$EVID" && pwd)
rm -f "$EVID"/{steamhost.log,wine.log,steamhost-wine.log,seed.log,webhelper_samples,webhelper_count,exit-code,probe.log,probe-wine.log,Player.log,game-wine.log,logon.txt}
rm -rf "$EVID/steam-logs" "$EVID/uwph-reports"
# Fetch through the existing manifest/SHA2 verifier only if not already staged.
if [[ ! -f $HERE/out/client/steamclient64.dll ]]; then
    FREE=$(df --output=avail -B1 / | tail -1)
    ((FREE >= 10*1024*1024*1024)) || { echo 'need >=10GiB free before download' >&2; exit 1; }
    python3 "$MIN/fetch-valve-client.py" --out "$HERE/out/client" >"$EVID/fetch.log"
fi
make -C "$HERE"
[[ $MODE == stock ]] || make -C "$HARNESS" out/libuwph_wine_shim.so out/run_via_wine out/wine-wx/.stamp
REPORTS=$MIN/out/uwph-reports
rm -rf "$REPORTS"
[[ $MODE == stock ]] || mkdir -p "$REPORTS"
# guest TAG: sets G to the command prefix that runs one guest PE on this route.
guest() {
    if [[ $MODE == uwph ]]; then
        G=(env UWPH_GUEST_RW="$MIN/out" UWPH_REPORT="$REPORTS/$1.report" "$HARNESS/run-wine.sh" --guest)
    else
        G=(wine)
    fi
}
# Under steam-min/out/ (gitignored): seed-login.py only writes below that directory.
PFX=$(mktemp -d "$MIN/out/steamhost-pfx.XXXXXXXX")
export WINEPREFIX=$PFX WINEARCH=win64 WINEDEBUG=-all
export WINEDLLOVERRIDES='mscoree,mshtml='
XVFB= REC= SAMPLER= HOST=
cleanup() {
    [[ -z $HOST ]] || kill "$HOST" 2>/dev/null || true
    [[ -z $SAMPLER ]] || kill "$SAMPLER" 2>/dev/null || true
    [[ -z $REC ]] || { kill -INT "$REC" 2>/dev/null || true; wait "$REC" 2>/dev/null || true; }
    wineserver -k 2>/dev/null || true
    wineserver -w 2>/dev/null || true
    [[ -z $XVFB ]] || kill "$XVFB" 2>/dev/null || true
    [[ $MODE == stock || ! -d $REPORTS ]] || { rm -rf "$EVID/uwph-reports"; cp -a "$REPORTS" "$EVID/uwph-reports"; }
    rm -rf "$PFX" "$MIN/out/megabonk-farm" "$REPORTS"
}
trap cleanup EXIT
# -displayfd asks Xvfb itself to allocate a free display.
Xvfb -displayfd 3 -screen 0 1280x720x24 -nolisten tcp 3>"$PFX/display" >"$PFX/xvfb.log" 2>&1 &
XVFB=$!
for _ in {1..50}; do [[ ! -s $PFX/display ]] || break; sleep .1; done
[[ -s $PFX/display ]] || { echo 'Xvfb gave no display' >&2; exit 1; }
export DISPLAY=:$(cat "$PFX/display")
timeout 60 wineboot -i >"$EVID/wineboot.log" 2>&1
wineserver -w
STEAM="$PFX/drive_c/Program Files (x86)/Steam"
mkdir -p "$STEAM"
cp -a "$HERE/out/client/." "$STEAM/"
cp "$HERE/out/steamhost64.exe" "$STEAM/"
# Valve's documented steam.cfg: no bootstrapper self-update.
printf 'BootStrapperInhibitAll=enable\nBootStrapperForceSelfUpdate=disable\n' >"$STEAM/steam.cfg"
if [[ $MODE == uwph ]]; then
    printf 'route=THROUGH-uwph (run-wine.sh --guest: uwp_run.sh wx,noproc -> run_via_wine -> Wine loader -> shim); nofork OFF; stock external wineserver\nscope=B74 O1 steamhost%s; Linux harness, no console proof\n' \
        "${GAME:+ logon, probe and Megabonk}" >"$EVID/route.txt"
elif [[ $SELFTEST == 1 ]]; then
    printf 'route=stock Wine\nscope=O1 build-only plus loader selftest; no uwph or console proof\n' >"$EVID/route.txt"
elif [[ $GAME == 1 ]]; then
    printf 'route=stock Wine\nscope=B73 O1 steamhost logon, probe and Megabonk; no uwph or console proof\n' >"$EVID/route.txt"
else
    printf 'route=stock Wine\nscope=O2 steamhost logon attempt; no uwph or console proof\n' >"$EVID/route.txt"
fi
# Show only the credential-free steamhost log in the short Xvfb video.
touch "$EVID/steamhost.log"
touch "$EVID/probe.log"
xterm -geometry 110x30 -e bash -c 'tail -F "$@"' _ "$EVID/steamhost.log" ${GAME:+"$EVID/probe.log"} >/dev/null 2>&1 &
record() {
    ffmpeg -nostdin -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" -t "$1" -c:v libx264 -preset ultrafast -pix_fmt yuv420p "$VIDEO" >"$EVID/video.log" 2>&1 &
    REC=$!
}
# --game records the game stage (<=60 s); before logon it only records if no game stage follows.
[[ $GAME == 1 ]] || record 55
sleep 1
set +e
if [[ $SELFTEST == 1 ]]; then
    guest selftest
    timeout 45 "${G[@]}" "$STEAM/steamhost64.exe" --selftest >"$EVID/steamhost.log" 2>"$EVID/$HWL"
    RC=$?
else
    # webhelper gate (João's no-browser rule): most RUNNING steamwebhelper* processes seen.
    ( while :; do ps -eo comm= | grep -ci '^steamwebhelper' || :; sleep 1; done ) >"$EVID/webhelper_samples" 2>/dev/null &
    SAMPLER=$!
    # Gate first: no credential is touched unless the vtable tables match the pinned build.
    guest steamhost-gate
    timeout 45 "${G[@]}" "$STEAM/steamhost64.exe" --check-pin >>"$EVID/steamhost.log" 2>"$EVID/$HWL"
    RC=$?
    if [[ $RC == 0 ]]; then
        mkdir -p "$MIN/out/bin" && x86_64-w64-mingw32-gcc -O2 -Wall -o "$MIN/out/bin/dpapi_protect.exe" "$MIN/dpapi_protect.c" -lcrypt32 &&
        python3 "$MIN/seed-login.py" --prefix "$PFX" --helper "$MIN/out/bin/dpapi_protect.exe" >"$EVID/seed.log" 2>&1
        if ! grep -qx 'seeded=1' "$EVID/seed.log"; then
            RC=10
        elif [[ $GAME != 1 ]]; then
            guest steamhost
            ( cd "$STEAM" && timeout $((WAIT + 60)) "${G[@]}" "$STEAM/steamhost64.exe" --logon --wait "$WAIT" ) \
                >>"$EVID/steamhost.log" 2>>"$EVID/$HWL"
            RC=$?
        else
            # steamhost stays the master (--serve) while probe and game run, then exits by itself.
            SERVE=$((GAME_SECS + 150))
            guest steamhost
            ( cd "$STEAM" && exec timeout $((WAIT + SERVE + 60)) "${G[@]}" "$STEAM/steamhost64.exe" --logon --wait "$WAIT" --serve "$SERVE" ) \
                >>"$EVID/steamhost.log" 2>>"$EVID/$HWL" &
            HOST=$!
            for ((i = 0; i < WAIT + 60; ++i)); do
                grep -q '^activeprocess_pid_is_self=' "$EVID/steamhost.log" || ! kill -0 "$HOST" 2>/dev/null && break
                sleep 1
            done
            if grep -q '^logon=1' "$EVID/steamhost.log" && grep -q '^serving=' "$EVID/steamhost.log"; then
                # Probe: public flat Steamworks API through Megabonk's own unmodified steam_api64.dll.
                mkdir -p "$MIN/out/bin"
                x86_64-w64-mingw32-gcc -O2 -Wall -o "$MIN/out/bin/steamapi_probe.exe" "$MIN/steamapi_probe.c"
                API_W=$(winepath -w "$GAME_SRC/Megabonk_Data/Plugins/x86_64/steam_api64.dll")
                guest probe
                timeout 60 "${G[@]}" "$MIN/out/bin/steamapi_probe.exe" "$API_W" 2>"$EVID/probe-wine.log" |
                    tr -d '\r' >"$EVID/probe.log"
                # Megabonk from a symlink farm (install untouched), -force-d3d11, GAME_SECS.
                FARM=$MIN/out/megabonk-farm
                rm -rf "$FARM"
                cp -as "$GAME_SRC" "$FARM"
                PLAYER_W=$(winepath -w "$PFX/Player.log")
                record "$GAME_SECS"
                guest game
                ( cd "$FARM" && SteamAppId=$APPID SteamGameId=$APPID timeout -s KILL "$GAME_SECS" "${G[@]}" "$FARM/Megabonk.exe" \
                    -force-d3d11 -screen-fullscreen 0 -screen-width 1280 -screen-height 450 -logFile "$PLAYER_W" ) \
                    >"$EVID/game-wine.log" 2>&1
                echo "megabonk_exit=$?" >"$EVID/logon.txt"
                [[ ! -f $PFX/Player.log ]] || cp "$PFX/Player.log" "$EVID/Player.log"
                wait "$REC" 2>/dev/null; REC=
            else
                printf 'BLOCKED: steamhost gave no logon=1 within %ss\nProbe and game skipped.\n' "$WAIT" >"$EVID/logon.txt"
                record 5; wait "$REC" 2>/dev/null; REC=
            fi
            kill "$HOST" 2>/dev/null; wait "$HOST" 2>/dev/null; HOST=
            # 9 = steamhost's own no-logon code; 11 = logged on but the game's steam_api did not init.
            if ! grep -q '^logon=1' "$EVID/steamhost.log"; then RC=9
            elif ! grep -q '^SteamAPI_Init=1' "$EVID/probe.log"; then RC=11
            else RC=0; fi
        fi
    else
        echo "run.sh: steamhost gate rc=$RC; seed-login.py not run, no credential read" >>"$EVID/$HWL"
        if [[ $GAME == 1 ]]; then
            printf 'BLOCKED: steamhost gate rc=%s; no logon, probe and game skipped\n' "$RC" >"$EVID/logon.txt"
            record 5; wait "$REC" 2>/dev/null; REC=
        fi
    fi
    kill "$SAMPLER" 2>/dev/null; wait "$SAMPLER" 2>/dev/null; SAMPLER=
    if [[ -s $EVID/webhelper_samples ]]; then
        sort -n "$EVID/webhelper_samples" | tail -1 >"$EVID/webhelper_count"
    else
        echo nosamples >"$EVID/webhelper_count"
    fi
    [[ -n $(find "$PFX" -iname 'steamwebhelper*' -o -iname 'libcef*' | head -1) ]] && echo cef-present >"$EVID/webhelper_count"
    # Redacted copies of Valve's logs (account name / SteamID never leave the prefix).
    LU="$STEAM/config/loginusers.vdf"
    if [[ -d $STEAM/logs ]]; then
        SID=$(sed -n 's/^\t"\([0-9]\{17\}\)"$/\1/p' "$LU" 2>/dev/null | head -1)
        ACC=$(sed -n 's/^\t\t"AccountName"\t\t"\(.*\)"$/\1/p' "$LU" 2>/dev/null | head -1)
        AID=$(( ${SID:-76561197960265728} - 76561197960265728 ))
        mkdir -p "$EVID/steam-logs"
        for f in "$STEAM"/logs/*.txt; do
            [[ -f $f ]] || continue
            if [[ -n $SID && -n $ACC ]]; then
                sed -e "s/$SID/<steamid>/g" -e "s/\[U:1:$AID\]/[U:1:<id>]/g" -e "s/$AID/<id>/g" -e "s/$ACC/<account>/gI" "$f"
            else cat "$f"; fi >"$EVID/steam-logs/$(basename "$f")"
        done
        [[ -z $SID || -z $ACC ]] || for f in "$EVID"/{wine.log,steamhost-wine.log,probe-wine.log,game-wine.log,Player.log}; do
            [[ ! -f $f ]] || sed -i -e "s/$SID/<steamid>/g" -e "s/\[U:1:$AID\]/[U:1:<id>]/g" -e "s/$ACC/<account>/gI" "$f"
        done
    fi
fi
set -e
cat "$EVID/steamhost.log"
printf '%s\n' "$RC" >"$EVID/exit-code"
sleep 2
exit "$RC"
