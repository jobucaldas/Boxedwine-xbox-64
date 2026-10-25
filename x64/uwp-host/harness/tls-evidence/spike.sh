#!/bin/bash
# B44 O2 spike, Linux stand-in only; no credentials, certificate validation ON.
# Host GnuTLS is refused (UWPH_DENY_DL=gnutls) and Wine's libgnutls.so.30
# dlopens are routed to the provider uwph builds (tls/build-gnutls.sh linux).
# Run 1: the O1 net_probe (WinHTTP GetCMList).  Run 2: tls/cert_probe.exe,
# good host plus bad chains that must be rejected.  Run 3 (control): deny
# without the provider, i.e. O1's absent-provider failure.
set -euo pipefail
repo=$(cd "$(dirname "$0")/../../../.." && pwd)
uwph=$repo/x64/uwp-host
E=${B44_EVIDENCE:-/workspace/x64-run/lanes/B44/o2}
assets=${B44_WINE_ASSETS:-$uwph/harness/out}
provider=$uwph/tls/out/linux/lib/libgnutls.so.30
[[ -e $provider ]] || "$uwph/tls/build-gnutls.sh" linux
provider=$(realpath "$provider")
mkdir -p "$E" /workspace/x64-run/videos
E=$(realpath "$E")
make -s -C "$uwph/harness" out/libuwph_wine_shim.so
cp /workspace/xbox-readiness/bin/net_probe.exe "$E/net_probe.exe"
x86_64-w64-mingw32-gcc -municode -O2 -Wall -Wextra -Werror -Wno-unused-parameter "$uwph/tls/cert_probe.c" -lwinhttp -o "$E/cert_probe.exe"
cc -O2 -Wall -Wextra -Werror "$repo/x64/pe/launcher/run_via_wine.c" -o "$E/run_via_wine"
for n in $(seq 250 299); do
 if [[ ! -e /tmp/.X11-unix/X$n && ! -e /tmp/.X$n-lock ]]; then break; fi
done
export DISPLAY=:$n
Xvfb "$DISPLAY" -screen 0 1024x768x24 -nolisten tcp > "$E/xvfb.log" 2>&1 & xp=$!
trap 'kill "$xp" 2>/dev/null || true' EXIT
sleep 1
ffmpeg -y -loglevel error -f x11grab -video_size 1024x768 -framerate 10 -i "$DISPLAY" -t 55 -c:v libx264 -threads 1 -pix_fmt yuv420p /workspace/x64-run/videos/B44-O2.mp4 > "$E/ffmpeg.log" 2>&1 & vp=$!
printf 'B44 O2 Linux stand-in: launcher -> Wine own loader -> uwph shim\nUWPH_DENY_DL=gnutls (host GnuTLS refused) + UWPH_TLS_PROVIDER=uwph-built GnuTLS 3.8.9\nCertificate validation ON; no Xbox console PASS claim\n' > "$E/video.txt"
xmessage -geometry 1000x240+5+5 -file "$E/video.txt" >/dev/null 2>&1 &
run() { # mode exe args...
 local mode=$1; shift
 if [[ -e "$E/prefix-$mode" ]]; then mv "$E/prefix-$mode" "$E/prefix-$mode.previous-$(date +%s)"; fi
 cp -a --reflink=auto /workspace/megabonk/pfx-stock "$E/prefix-$mode"
 export WINEPREFIX="$E/prefix-$mode" WINEDLLOVERRIDES=wineboot.exe=d WINEDEBUG=+winhttp,+schannel,+loaddll
 /usr/lib/wine/wineserver64 -f -p > "$E/$mode-server.log" 2>&1 & sp=$!
 sleep .3
 set +e
 WINESERVER=/bin/false WINE_LOADER="$assets/wine-wx/lib/wine/wine64" \
 timeout -k 3 24 strace -f -s 256 -e trace=process,openat -o "$E/$mode.exec" \
 env UWPH_DENY_DL=gnutls UWPH_TLS_PROVIDER="${P-$provider}" LD_PRELOAD="$uwph/harness/out/libuwph_wine_shim.so" UWPH_REPORT="$E/$mode.counters" UWPH_REPORT_PID=1 \
 "$E/run_via_wine" "$@" > "$E/$mode.log" 2> "$E/$mode-debug.log"
 rc=$?
 set -e
 echo "$rc" > "$E/$mode.exit"
 /usr/lib/wine/wineserver64 -k || true
 wait "$sp" || true
 xmessage -geometry 1000x420+5+260 -file "$E/$mode.log" >/dev/null 2>&1 &
}
run deny-provider "$E/net_probe.exe"
run cert-control "$E/cert_probe.exe" badssl.com self-signed.badssl.com expired.badssl.com untrusted-root.badssl.com wrong.host.badssl.com
wait "$vp"
# Control, after the video: the O1 knob alone still refuses GnuTLS (HTTP 0).
P= run deny-only "$E/net_probe.exe"
# Provider-only: no host TLS library may have been opened by any Wine process.
{ echo "host libgnutls/nettle/hogweed/gmp/tasn1 opens (must be none):"
  grep -hE 'openat\(.*/usr/lib[^"]*/lib(gnutls|nettle|hogweed|gmp|tasn1|p11-kit)[^"]*"' "$E"/deny-provider.exec "$E"/cert-control.exec | grep -v ENOENT || echo none
  echo "provider file opens:"; grep -c "$provider" "$E"/deny-provider.exec "$E"/cert-control.exec || true
  echo "Wine libgnutls.so.30 dlopens routed to provider:"; grep -c 'UWPH_TLS_PROVIDER: .*(loaded)' "$E"/deny-provider-debug.log "$E"/cert-control-debug.log || true
  echo "deny-only control refusals:"; grep -c 'UWPH_DENY_DL=gnutls: refused' "$E"/deny-only-debug.log || true
  readelf -d "$provider" | grep -E 'NEEDED|SONAME'
} > "$E/provider-isolation.txt"
sha256sum "$E/net_probe.exe" "$E/cert_probe.exe" "$E/run_via_wine" "$uwph/harness/out/libuwph_wine_shim.so" "$assets/wine-wx/lib/wine/wine64" "$provider" > "$E/artifacts.sha256"
