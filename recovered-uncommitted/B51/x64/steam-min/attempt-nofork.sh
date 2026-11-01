#!/usr/bin/env bash
# B51 O2: no credentials; bounded startup THROUGH uwph, not a login proof.
set -euo pipefail
umask 077
here=$(cd "$(dirname "$0")" && pwd)
harness=$here/../uwp-host/harness
evid=/workspace/x64-run/lanes/B51/o2
work=$here/out/nofork
mkdir -p "$evid" "$work" /workspace/x64-run/videos
[[ ! -e $work/prefix ]] # Preserve evidence: require a fresh staged prefix.
cp -a --reflink=auto "$harness/prebuilt-prefix" "$work/prefix"
chmod -R u+w "$work/prefix"
steam="$work/prefix/drive_c/Program Files (x86)/Steam"
mkdir -p "$steam"
cp -a "$here/out/client/." "$steam/"
printf 'BootStrapperInhibitAll=enable\nBootStrapperForceSelfUpdate=disable\n' > "$steam/steam.cfg"
for n in {170..239}; do
    if [[ ! -e /tmp/.X11-unix/X$n && ! -e /tmp/.X$n-lock ]]; then export DISPLAY=:$n; break; fi
done
Xvfb "$DISPLAY" -screen 0 1280x720x24 -nolisten tcp > "$evid/xvfb.log" 2>&1 &
xpid=$!
trap 'kill "$xpid" 2>/dev/null || true' EXIT
sleep 1
export WINEPREFIX=$work/prefix UWPH_GUEST_RW=$work UWPH_REPORT=$work/report
export WINEDEBUG=+process,+loaddll,+sync,+winsock
printf 'THROUGH run-wine.sh --guest --inproc-server -> uwpjail nofork,wx,noproc -> launcher -> private Wine loader -> uwph\ncredentials=NONE display=%s timeout=30s\n' "$DISPLAY" > "$evid/nofork-route.txt"
rc=0
(cd "$steam"; timeout -k 5 30 strace -ff -s 256 -e trace=process -o "$evid/nofork-syscalls" "$harness/run-wine.sh" --guest --inproc-server "$steam/steam.exe" -silent -nofriendsui -nochatui -nobigpicture -nointro -noverifyfiles) > "$evid/nofork-wine.log" 2>&1 || rc=$?
printf 'exit=%s\n' "$rc" >> "$evid/nofork-route.txt"
for report in "$work"/report*; do [[ ! -f $report ]] || cp "$report" "$evid/"; done
[[ ! -d $steam/logs ]] || cp -a "$steam/logs" "$evid/steam-logs"
{ cat "$evid/nofork-route.txt"; grep -E 'inproc|Operation not permitted|err:|failed|seccomp' "$evid/nofork-wine.log" | head -20 || true; } > "$evid/nofork-video.txt"
xterm -geometry 125x30+0+0 -T B51-O2 -e bash -c 'cat "$1"; read -r' bash "$evid/nofork-video.txt" >/dev/null 2>&1 &
vpid=$!
ffmpeg -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" -t 8 -c:v libx264 -preset veryfast -pix_fmt yuv420p /workspace/x64-run/videos/B51-O2.mp4
kill "$vpid" 2>/dev/null || true
! grep -rIl 'eyJ' "$evid" "$here"
