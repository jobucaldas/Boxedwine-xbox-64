#!/usr/bin/env bash
# B70 O2: Valve's steamcmd (unmodified, from Valve's manifest, sha2-verified) on STOCK Wine,
# `+login anonymous` (no token, no account), in a fresh small prefix with the B70 O2
# prefix fix (fix-prefix-tls.sh) applied unless --no-fix. Proves Valve's own HTTPS
# (GetCMListForConnect) and logon. Linux box, stock Wine only; never console proof.
#
# Usage: run-steamcmd-anon.sh --stock --evidence DIR [--no-fix] [--video FILE] [--wait SECS]
# Evidence: steamcmd-connection_log.txt (redacted), steamcmd-stderr.txt, steamcmd-output.txt,
#   steamcmd-manifest.txt, verify.log, prefix-tls-fix.log, result.txt.
set -euo pipefail
MODE= EVID= FIX=1 VIDEO= WAIT=180
while [ $# -gt 0 ]; do
    case $1 in
        --stock) MODE=stock; shift ;;
        --evidence) EVID=$2; shift 2 ;;
        --no-fix) FIX=0; shift ;;
        --video) VIDEO=$2; shift 2 ;;
        --wait) WAIT=$2; shift 2 ;;
        *) echo "run-steamcmd-anon.sh: unknown arg $1" >&2; exit 2 ;;
    esac
done
[ "$MODE" = stock ] && [ -n "$EVID" ] || { echo "usage: $0 --stock --evidence DIR [--no-fix] [--video FILE]" >&2; exit 2; }
HERE=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$EVID"; EVID=$(cd "$EVID" && pwd)
CACHE=$HERE/../out/steamcmd-cache
mkdir -p "$CACHE"
P=$(mktemp -d /tmp/B70-steamcmd-XXXXXX)
export WINEPREFIX=$P WINEARCH=win64 WINEDEBUG=-all WINEDLLOVERRIDES='mscoree,mshtml='
D=$P/drive_c/Steam
XVFB= FF= TAILP=
cleanup() {
    wineserver -k 2>/dev/null || :
    for p in $TAILP $FF $XVFB; do kill "$p" 2>/dev/null || :; done
    rm -rf "$P"
}
trap cleanup EXIT
rm -f "$EVID"/steamcmd-*.txt "$EVID"/{verify.log,result.txt,prefix-tls-fix.log,prefix-tls-fix.out}

# --- Valve's manifest + packages (CDN only, sha2-verified, used unmodified) -------------
CDN=https://client-update.steamstatic.com
curl -fsS -m 30 -o "$EVID/steamcmd-manifest.txt" "$CDN/steam_cmd_win32"
mkdir -p "$D"
python3 -I - "$EVID/steamcmd-manifest.txt" "$CACHE" "$D" "$CDN" /workspace/steamcmd-probe > "$EVID/verify.log" <<'PY'
import hashlib, pathlib, re, shutil, sys, urllib.request, zipfile
manifest, cache, dest, cdn, legacy = sys.argv[1:]
text = pathlib.Path(manifest).read_text()
for name in ("steamcmd_win32", "steamcmd_bins_win32", "steamcmd_public_all"):
    block = re.search(r'"%s"\s*\{([^}]+)' % name, text)[1]
    fn = re.search(r'"file"\s*"([^"]+)"', block)[1]
    want = re.search(r'"sha2"\s*"([0-9a-f]+)"', block)[1]
    path = pathlib.Path(cache) / fn
    if not path.exists() and (pathlib.Path(legacy) / fn).exists():
        shutil.copy(pathlib.Path(legacy) / fn, path)
    if not path.exists():
        with urllib.request.urlopen(f"{cdn}/{fn}", timeout=120) as r, open(path, "wb") as o:
            shutil.copyfileobj(r, o)
    got = hashlib.sha256(path.read_bytes()).hexdigest()
    if got != want:
        sys.exit(f"{name}: sha2 MISMATCH {got} != {want}")
    with zipfile.ZipFile(path) as z:
        z.extractall(dest)
    print(f"{name} file={fn} sha2={got} verified=1")
PY
cat "$EVID/verify.log"
printf 'BootStrapperInhibitAll=enable\n' > "$D/steam.cfg"

# --- private Xvfb, fresh prefix ---------------------------------------------------------
for d in $(seq 170 239); do
    [ -e /tmp/.X11-unix/X$d ] || [ -e /tmp/.X$d-lock ] || { DNUM=$d; break; }
done
Xvfb ":$DNUM" -screen 0 1280x720x24 -nolisten tcp >/dev/null 2>&1 & XVFB=$!
export DISPLAY=:$DNUM
sleep 1
timeout 90 wineboot -u >/dev/null 2>&1
wineserver -w
if [ "$FIX" = 1 ]; then
    bash "$HERE/fix-prefix-tls.sh" --prefix "$P" --evidence "$EVID" > "$EVID/prefix-tls-fix.out" 2>&1 \
        || echo "prefix_tls_fix=partial" >> "$EVID/prefix-tls-fix.log"
    FIXDESC="fix-prefix-tls.sh applied"
else
    FIXDESC="no fix (baseline)"
fi

# --- steamcmd +login anonymous ------------------------------------------------------------
mkdir -p "$D/logs"; : > "$D/logs/connection_log.txt"
if [ -n "$VIDEO" ]; then
    mkdir -p "$(dirname "$VIDEO")"
    xterm -geometry 160x48+0+0 -fa Mono -fs 8 -T "B70-O2 steamcmd anonymous, stock Wine, $FIXDESC" -e \
        "tail -n +1 -F '$D/logs/connection_log.txt' | sed -u -E 's/\\[([aAUg]):1:[0-9]+\\]/[\\1:1:<id>]/g'" \
        >/dev/null 2>&1 & TAILP=$!
    ffmpeg -nostdin -y -loglevel error -f x11grab -video_size 1280x720 -framerate 10 -i "$DISPLAY" \
        -t 58 -c:v libx264 -preset veryfast -pix_fmt yuv420p "$VIDEO" & FF=$!
fi
mkfifo "$P/in"; exec 9<>"$P/in"
T0=$(date +%s)
( cd "$D" && timeout $((WAIT + 30)) wine steamcmd.exe +@NoPromptForPassword 1 +login anonymous \
    < "$P/in" > "$P/steamcmd.out" 2>&1 ) &
CP=$!
LOGON=0
for _ in $(seq 1 "$WAIT"); do
    grep -aq '\[Logged On' "$D/logs/connection_log.txt" 2>/dev/null && { LOGON=1; break; }
    sleep 1
done
T=$(( $(date +%s) - T0 ))
sleep 3
printf 'quit\n' >&9
sleep 5
wineserver -k 2>/dev/null || :
wait "$CP" 2>/dev/null || :
[ -n "$FF" ] && { wait "$FF" 2>/dev/null || :; FF=; }

# --- redacted evidence (anonymous ids / any 17-digit SteamID) ------------------------------
python3 -I - "$D" "$EVID" "$P" <<'PY'
import pathlib, re, sys
d, e, p = map(pathlib.Path, sys.argv[1:])
for src, dst in ((d / "logs/connection_log.txt", "steamcmd-connection_log.txt"),
                 (d / "logs/stderr.txt", "steamcmd-stderr.txt"),
                 (p / "steamcmd.out", "steamcmd-output.txt")):
    if src.exists():
        s = src.read_text(errors="replace")
        s = re.sub(r"\[([aAUg]):1:\d+\]", r"[\1:1:<id>]", s)
        s = re.sub(r"\b\d{17}\b", "<steamid>", s)
        (e / dst).write_text(s)
PY
C=$EVID/steamcmd-connection_log.txt
CMLIST_OK=$(grep -ac 'GetCMListForConnect -- got ' "$C" || :)
STATUS0=$(grep -ac 'web API call failed (status = 0)' "$C" || :)
{
    echo "route=stock Wine $(wine --version 2>/dev/null); $FIXDESC"
    echo "logon=$LOGON after ${T}s"
    echo "getcmlist_ok_lines=$CMLIST_OK"
    echo "status0_lines=$STATUS0"
    grep -a -m3 -E 'GetCMListForConnect -- (got|web API)' "$C" || :
    grep -a -m1 '\[Logged On' "$C" || :
} | tee "$EVID/result.txt"
du -sh "$P" | sed 's/^/prefix_size=/' >> "$EVID/result.txt"
[ "$LOGON" = 1 ] && [ "$CMLIST_OK" -gt 0 ] && [ "$STATUS0" = 0 ]
