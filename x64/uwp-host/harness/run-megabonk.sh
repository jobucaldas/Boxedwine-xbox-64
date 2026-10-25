#!/usr/bin/env bash
# B11/B12: owned Megabonk THROUGH uwp_run.sh -> our launcher -> Wine's own
# (W^X-patched) loader/ntdll.so -> uwph shim -> Linux backend.
# Prints `MEGABONK reached=<stage>`; stage is the furthest of
# loader|unityplayer|gfx-init|first-frame|menu (none if Wine never loaded a DLL).
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
secs=${MEGABONK_TIMEOUT:-120}
wrapper=${UWPH_UWP_RUN:-/workspace/xbox-readiness/tests/uwp_run.sh}
# Visual template for the main menu: logo crop of an earlier owned run.
menu_ref=${MEGABONK_MENU_REF:-/workspace/megabonk/shots/2-xbox-launcher-run_via_wine-menu.png}
: "${DISPLAY:?run-megabonk.sh needs DISPLAY (private Xvfb)}"
[[ -f $game/Megabonk.exe && -f $game/GameAssembly.dll ]]

make -s out/libuwph_wine_shim.so out/run_via_wine out/wine-wx/.stamp
out=$here/out
work=$out/megabonk
mkdir -p "$work"
exec 9>"$work/lock"
flock -n 9
if [[ ! -d $work/prefix ]]; then
    cp -a --reflink=auto /workspace/megabonk/pfx-stock "$work/prefix"
fi
run=$(mktemp -d "$work/run-XXXXXXXX")
echo "run=$run"
echo "constraints=$constraints (all three required; enforcement evidence in wine.log)"
echo "IL2CPP: $(ls "$game"/GameAssembly.dll) present; no Mono JIT runtime is shipped"

export WINEPREFIX=$work/prefix EXTRA_RW=$out WINEDLLOVERRIDES=wineboot.exe=d
# Retained concession (as run-acceptance.sh): stock Linux server outside the jail.
/usr/lib/wine/wineserver64 -p 180 || true
export WINESERVER=/bin/false
cleanup() { /usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true; }
trap cleanup EXIT

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
# first-frame: a captured window image with more than one colour.
best=
for png in "$run"/shot-*.png; do
    [[ -f $png ]] || continue
    colors=$(identify -format '%k' "$png" 2>/dev/null || echo 0)
    if [[ $stage == gfx-init || $stage == first-frame ]] && (( colors > 1 )); then
        stage=first-frame best=$png
    fi
done
# menu: the static MEGABONK logo region matches the earlier menu capture.
if [[ $stage == first-frame && -f $menu_ref ]]; then
    for png in "$run"/shot-*.png; do
        err=$(compare -metric RMSE <(convert "$png" -resize '1280x720!' -crop 440x100+420+75 png:-) \
              <(convert "$menu_ref" -crop 440x100+420+75 png:-) null: 2>&1 | sed -n 's/.*(\([0-9.e+-]*\)).*/\1/p' || true)
        echo "menu-logo RMSE $(basename "$png") ${err:-?}" >> "$run/menu.txt"
        if [[ -n $err ]] && awk -v e="$err" 'BEGIN { exit !(e < 0.10) }'; then stage=menu best=$png; fi
    done
fi
[[ -n $best ]] && cp "$best" "$run/best.png"

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
grep -m5 'err:\|Operation not permitted' "$log" || true
tail -n 3 "$player" | sed 's/^/player: /'
echo "evidence: $run (wine.log, player.log, shot-*.png${best:+, best.png})"
echo "THROUGH uwp_run.sh($constraints) -> launcher -> Wine own W^X loader -> shim -> uwph; separate stock wineserver"
echo "MEGABONK reached=$stage"
