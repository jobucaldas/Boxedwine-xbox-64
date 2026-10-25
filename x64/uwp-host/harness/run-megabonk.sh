#!/usr/bin/env bash
# B11/B12: owned Megabonk THROUGH uwp_run.sh -> our launcher -> Wine's own
# (W^X-patched) loader/ntdll.so -> uwph shim -> Linux backend.
# Prints `MEGABONK reached=<stage>`; stage is the furthest of
# loader|unityplayer|gfx-init (none if Wine never loaded a DLL).
# Screenshots are diagnostic only; presentation is not instrumented.
# B28: the prefix is a fresh copy of the B19 packaged prebuilt-prefix and Wine's
# own server is hosted in the game process (UWPH_INPROC_SERVER=1). That server
# validates .update-timestamp and pre-creates __wineboot_event, so ntdll's
# run_wineboot waits on it instead of spawning wineboot.exe under nofork.
# Requires DISPLAY (a private Xvfb). Linux proof only; Xbox untested.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
here=$PWD
game=${MEGABONK_DIR:-/workspace/steam-owned/games/megabonk}
constraints=${UWPH_CONSTRAINTS:-nofork,wx,noproc}
# B12 requires every constraint; reject partial runs before starting Wine.
for required in nofork wx noproc; do
    case ",$constraints," in
        *",$required,"*) ;;
        *) echo "B12 requires nofork,wx,noproc (missing $required)" >&2; exit 2 ;;
    esac
done
secs=${K_TIMEOUT:-${MEGABONK_TIMEOUT:-120}}
wrapper=${UWPH_UWP_RUN:-/workspace/xbox-readiness/tests/uwp_run.sh}
: "${DISPLAY:?run-megabonk.sh needs DISPLAY (private Xvfb)}"
[[ -f $game/Megabonk.exe && -f $game/GameAssembly.dll ]]

make -s out/libuwph_wine_shim.so out/run_via_wine out/wine-wx/.desk-stamp
# Packaging-time step (stock wineboot, outside the jail); reused when current.
make -s prebuilt-prefix
out=$here/out
work=$out/megabonk
mkdir -p "$work"
exec 9>"$work/lock"
flock -n 9
# Re-stage every run (one copy kept) so game writes never age the packaged state.
( source ./prefix-hygiene.sh; refuse_dereferenced_prefix "$here/prebuilt-prefix" )
if [[ -d $work/prefix ]]; then chmod -R u+w "$work/prefix"; rm -rf "$work/prefix"; fi
cp -a --reflink=auto "$here/prebuilt-prefix" "$work/prefix"
chmod -R u+w "$work/prefix"
cp "$out/wine-wx/lib/wine/x86_64-windows/uwpdesktop.dll" "$work/prefix/drive_c/windows/system32/uwpdesktop.dll"
run=$(mktemp -d "$work/run-XXXXXXXX")
printf 'Copied read-only prebuilt prefix: %s\n' "$here/prebuilt-prefix" > "$run/fixture.txt"
echo "run=$run"
echo "constraints=$constraints (all three required; enforcement evidence in wine.log)"
echo "IL2CPP: $(ls "$game"/GameAssembly.dll) present; no Mono JIT runtime is shipped"

# Hosted in-process server (linked into the shim); uwp_run.sh then skips its
# stock-server concession. WINESERVER is only a never-taken fallback.
unset WINEDLLOVERRIDES UWPH_INPROC_OWNER
export WINEPREFIX=$work/prefix EXTRA_RW=$out UWPH_INPROC_SERVER=1 WINE_INPROC_DESKTOP=1 WINESERVER=/bin/false

rc=0
timeout --kill-after=5s "${secs}s" "$wrapper" "$constraints" "$game" -- env \
    LD_PRELOAD="$out/libuwph_wine_shim.so" UWPH_REPORT="$run/report" UWPH_REPORT_PID=1 \
    WINE_LOADER="$out/wine-wx/lib/wine/wine64" WINEDEBUG=+loaddll,+process,err \
    "$out/run_via_wine" "$game/Megabonk.exe" -force-d3d11 -screen-fullscreen 0 \
    -logFile "$run/player.log" > "$run/wine.log" 2>&1 &
gpid=$!

# Observe from outside the jail: periodic screenshots of the game window, then
# ask the game to quit (Alt+F4 -> WM_CLOSE) so the main process flushes its report.
win=
shot=0
for ((t = 0; t < secs - 20; t += 5)); do
    sleep 5
    kill -0 "$gpid" 2>/dev/null || break
    [[ -n $win ]] || win=$(xdotool search --name '^Megabonk$' 2>/dev/null | head -n1 || true)
    if [[ -n $win ]] && xwd -silent -id "$win" > "$run/shot.xwd" 2>/dev/null; then
        shot=$((shot + 1))
        convert "$run/shot.xwd" "$run/shot-$(printf %03d "$shot").png" 2>/dev/null || true
    fi
done
if kill -0 "$gpid" 2>/dev/null && [[ -n $win ]]; then
    xdotool windowfocus --sync "$win" key alt+F4 2>/dev/null || true
    for _ in {1..15}; do kill -0 "$gpid" 2>/dev/null || break; sleep 1; done
fi
wait "$gpid" || rc=$?
echo "exit=$rc (124 = ${secs}s timeout)"

# Stage evidence.
stage=none
log=$run/wine.log player=$run/player.log
touch "$player"
grep -q 'loaddll:.*Loaded' "$log" && stage=loader
{ grep -q 'Loaded L".*UnityPlayer.dll"' "$log" || grep -q '^Initialize engine version' "$player"; } && stage=unityplayer
grep -q '^    Version:  Direct3D 11' "$player" && stage=gfx-init
# A multicolour screenshot (or matching logo) does not prove that the game
# presented a frame: window borders, startup/error UI and unpresented surfaces
# can look similar. Until actual game presentation is instrumented, retain
# gfx-init even when screenshots exist. Keep captures as diagnostic evidence.

# Routing + surface: main-process reports written by the shim's _exit hook.
reports=("$run"/report.*)
if [[ -f ${reports[0]} ]]; then
    echo "uwph reports: ${#reports[@]} ($(cd "$run" && echo report.*))"
    cat "${reports[@]}" | awk '$1=="total" { n[$2]+=$3 } END { for (k in n) printf "%s=%d ", k, n[k]; print "" }' | sed 's/^/selected backend totals: /'
    cat "${reports[@]}" | awk '$1=="trace" { n[$2]+=$3 } END { for (k in n) print k "\t" n[k] }' | sort > "$run/trace.tsv"
    if [[ -s $run/trace.tsv ]]; then
        tag="B12 megabonk $constraints"
        grep -v -F "$tag" surface-trace.tsv > "$run/surface.tmp" || true
        awk -F'\t' -v tag="$tag" -v ex="$rc" '{ printf "-\tuwph-trace\t%s\t-\t-\t%s\t-\t%s; uwph entry counter, all reports, exit=%s\n", $1, $2, tag, ex }' \
            "$run/trace.tsv" >> "$run/surface.tmp"
        cat "$run/surface.tmp" > surface-trace.tsv
        echo "surface-trace.tsv: $(wc -l < "$run/trace.tsv") uwph-trace rows for '$tag'"
    fi
else
    echo "uwph reports: none (no process reached the shim _exit hook)"
fi
echo "CreateProcess attempts: $(grep -c 'CreateProcessInternalW' "$log" || true)"
# Count the actual NT entry trace, not CreateProcessInternalW diagnostic lines.
# Keep Wine's quoted/escaped image and command line intact in the inventory.
python3 - "$log" <<'PYSPAWN'
import re
import sys

quoted = r'L"(?:\\.|[^"\\])*"'
pattern = re.compile(r':trace:process:NtCreateUserProcess .*? image (' + quoted
                     + r') cmdline (' + quoted + r') parent ')
attempts = []
with open(sys.argv[1], encoding="utf-8", errors="replace") as stream:
    for line in stream:
        match = pattern.search(line)
        if match:
            attempts.append(match.groups())
internal = sum(image[2:-1].replace("\\\\", "\\").lower().startswith("c:\\windows\\")
               for image, _ in attempts)
print(f"NtCreateUserProcess attempts: {len(attempts)} (wine-internal: {internal}, guest: {len(attempts) - internal})")
for image, cmdline in attempts:
    print(f"spawn: {image} {cmdline}")
PYSPAWN
grep -m5 'err:\|Operation not permitted' "$log" || true
tail -n 3 "$player" | sed 's/^/player: /'
echo "evidence: $run (wine.log, player.log, shot-*.png)"
echo "wineboot spawn failures: $(grep -c 'failed to start wineboot' "$log" || true)"
echo "THROUGH uwp_run.sh($constraints) -> launcher -> Wine own W^X loader -> shim -> uwph; Wine server hosted in-process"
echo "MEGABONK reached=$stage"
