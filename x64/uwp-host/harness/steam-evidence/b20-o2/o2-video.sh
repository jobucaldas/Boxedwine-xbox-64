#!/bin/bash
# Xvfb :122 (free) + xterm running o2-uwph.sh; first 60 s recorded.
L=/workspace/x64-run/lanes/B20
Xvfb :122 -screen 0 1024x768x24 >"$L/o2-video-xvfb.log" 2>&1 & xp=$!
sleep 1; export DISPLAY=:122
ffmpeg -y -loglevel error -f x11grab -video_size 1024x768 -framerate 10 -i :122 -t 60 \
    -c:v libx264 -pix_fmt yuv420p /workspace/x64-run/videos/B20-O2.mp4 & fp=$!
xterm -fa Monospace -fs 11 -geometry 120x40+0+0 -e bash -c '
echo "B20 O2 (run 2): Steam client THROUGH run_via_wine -> Wine W^X loader -> uwph (new-style WoW64)"
echo "stock-Wine install already done:"; ls -la "/workspace/wt/B20/x64/uwp-host/harness/out/b20/pfx-wow64/drive_c/Program Files (x86)/Steam/Steam.exe"
/workspace/x64-run/lanes/B20/o2-uwph.sh 2>&1 | tee /workspace/x64-run/lanes/B20/o2-uwph-run2.out'
wait $fp; kill $xp
