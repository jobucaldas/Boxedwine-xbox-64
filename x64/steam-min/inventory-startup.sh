#!/usr/bin/env bash
# Credential-free B51 O1 startup observations, not a SteamAPI success test.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
EVID=/workspace/x64-run/lanes/B51/o1
mkdir -p "$EVID" /workspace/x64-run/videos
umask 077
for n in $(seq 170 239); do
    if [[ ! -e /tmp/.X11-unix/X$n && ! -e /tmp/.X$n-lock ]]; then
        export DISPLAY=:$n; break
    fi
done
Xvfb "$DISPLAY" -screen 0 1280x720x24 -nolisten tcp >"$EVID/xvfb.log" 2>&1 &
xpid=$!
trap 'kill "$xpid" 2>/dev/null || true' EXIT
sleep 1
for route in stock uwph; do
    dest=$EVID/$route
    mkdir -p "$dest" "$HERE/out/$route"
    export WINEPREFIX=$HERE/out/$route/pfx WINEARCH=win64
    WINEDEBUG=-all wineboot -i >"$dest/setup.log" 2>&1
    wineserver -w
    steam="$WINEPREFIX/drive_c/Program Files (x86)/Steam"
    mkdir -p "$steam"
    cp -a "$HERE/out/client/." "$steam/"
    printf 'BootStrapperInhibitAll=enable\nBootStrapperForceSelfUpdate=disable\n' > "$steam/steam.cfg"
    export WINEDEBUG=+process,+loaddll,+sync,+winsock
    cmd=(wine "$steam/steam.exe")
    if [[ $route == uwph ]]; then
        export UWPH_GUEST_RW=$HERE/out UWPH_REPORT=$HERE/out/$route/uwph.report
        cmd=("$HERE/../uwp-host/harness/run-wine.sh" --guest "$steam/steam.exe")
    fi
    echo "route=$route display=$DISPLAY nofork=OFF credentials=NONE duration=45s" > "$dest/route.txt"
    set +e
    (cd "$steam"; timeout -k 5 45 strace -ff -s 512 -e trace=process,bind,listen,getsockname,connect -o "$dest/syscalls" "${cmd[@]}" -silent -nofriendsui -nochatui -nobigpicture -nointro -noverifyfiles) > "$dest/wine.log" 2>&1
    echo "exit=$?" >> "$dest/route.txt"
    set -e
    WINEDEBUG=-all wineserver -k || true
    if [[ -d $steam/logs ]]; then cp -a "$steam/logs" "$dest/steam-logs"; fi
    for report in "$HERE/out/$route/"uwph.report*; do
        [[ ! -f $report ]] || cp "$report" "$dest/"
    done
    grep -E 'CreateProcessInternalW app|NtCreateUserProcess.*(pid | image )' "$dest/wine.log" > "$dest/processes.txt" || true
    grep 'CreateNamedPipeW' "$dest/wine.log" > "$dest/named-pipes.txt" || true
    grep -E 'trace:winsock:(bind|listen|getsockname)' "$dest/wine.log" > "$dest/ports.txt" || true
done
printf 'B51 O1: credential-free startup inventory\nStock Wine then THROUGH uwph (nofork OFF).\nB50 login failed: game IPC and SteamAPI dependency remain unproven.\n' > "$EVID/video.txt"
xterm -geometry 110x20+10+10 -T B51-O1 -e "cat '$EVID/video.txt'; read -r" > /dev/null 2>&1 &
vpid=$!
ffmpeg -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" -t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p /workspace/x64-run/videos/B51-O1.mp4
kill "$vpid" 2>/dev/null || true
