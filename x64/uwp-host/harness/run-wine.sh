#!/usr/bin/env bash
# Owned Linux-box harness only. Controls use stock Debian Wine; routed runs use
# the private W^X ntdll.so from prepare-wine-wx.sh (R4X O2), because the shim
# refuses WRITE|EXEC and stock ntdll maps PE images RWX.
set -euo pipefail
cd "$(dirname "$0")"
inproc=0
for arg in "$@"; do
    case "$arg" in
        --inproc-server) inproc=1 ;;
        --no-build) ;; # Builds are always performed by the Make targets.
        *) echo "Unknown argument: $arg" >&2; exit 2 ;;
    esac
done
root=$(realpath ../../..)
work=$PWD/out/wine-o4
mkdir -p "$work"
exec 9>"$work/lock"
flock -n 9 || { echo 'O4 already running' >&2; exit 1; }
[[ -f out/wine/identity.json ]] || ./prepare-wine.sh
python3 - <<'PY'
import hashlib,json,pathlib
identity=json.loads(pathlib.Path('out/wine/identity.json').read_text())
for item in identity['artifacts'].values():
    assert hashlib.sha256(pathlib.Path(item['path']).read_bytes()).hexdigest()==item['sha256'], 'Wine identity changed; run prepare-wine.sh'
PY
[[ -f out/wine-wx/.stamp ]] || ./prepare-wine-wx.sh
wx=$PWD/out/wine-wx
# Never use ~/.wine. Only copy an already initialized, explicit fixture.
if (( inproc )); then
    source_prefix=$PWD/prebuilt-prefix
    [[ -d $source_prefix ]] || { echo 'Run make prebuilt-prefix at packaging time' >&2; exit 1; }
    run=$(mktemp -d "$work/run-XXXXXXXX")
    cp -a --reflink=auto "$source_prefix" "$run/prefix"
    chmod -R u+w "$run/prefix"
    export WINEPREFIX=$run/prefix
    printf 'Copied read-only prebuilt prefix: %s\n' "$source_prefix" > "$run/fixture.txt"
else
source_prefix=${UWPH_FIXTURE_PREFIX:-/workspace/megabonk/pfx-stock}
if [[ ! -d $work/prefix ]]; then
    for f in system.reg user.reg userdef.reg .update-timestamp; do
        [[ -f $source_prefix/$f ]] || { echo "Missing initialized fixture: $source_prefix/$f" >&2; exit 1; }
    done
    cp -a --reflink=auto "$source_prefix" "$work/prefix.tmp"
    {
        echo "SETUP ONLY: copied initialized stock-Wine fixture from $(realpath "$source_prefix")"
        echo 'No wineboot command invoked by preparation; source prefix is never written.'
        sha256sum "$source_prefix/"{system.reg,user.reg,userdef.reg,.update-timestamp}
        echo 'Proof disables wineboot.exe; any attempted initialization still FAILS acceptance.'
    } > "$work/fixture.txt"
    mv "$work/prefix.tmp" "$work/prefix"
fi
export WINEPREFIX=$work/prefix
run=$(mktemp -d "$work/run-XXXXXXXX")
fi
cp out/wine/identity.json "$run/identity.json"
# Routed identity: private W^X Unix + PE ntdll; other DLLs are symlinks.
python3 - "$run/identity-wx.json" "$wx" <<'PY'
import hashlib,json,pathlib,sys
tree=pathlib.Path(sys.argv[2])
paths=dict(loader=tree/'lib/wine/wine64', pe_ntdll=tree/'lib/x86_64-linux-gnu/wine/x86_64-windows/ntdll.dll',
           unix_ntdll=tree/'lib/x86_64-linux-gnu/wine/x86_64-unix/ntdll.so')
def identity(p):
    return dict(path=str(p.resolve()),sha256=hashlib.sha256(p.read_bytes()).hexdigest())
data=dict(selection='private W^X Unix + PE ntdll (prepare-wine-wx.sh, image + stub patches); other DLLs = installed Debian Wine',
          stamp=(tree/'.stamp').read_text().strip(), artifacts={k:identity(v) for k,v in paths.items()})
pathlib.Path(sys.argv[1]).write_text(json.dumps(data,indent=2)+'\n')
PY
if (( ! inproc )); then cp "$work/fixture.txt" "$run/fixture.txt"; fi
cp out/wine/direct-syscalls.tsv "$run/direct-syscalls.tsv"
cp surface.tsv "$run/surface.tsv"
echo "O4 evidence: $run"
cat "$run/fixture.txt"
if (( inproc )); then
    export UWPH_INPROC_SERVER=1
    unset UWPH_INPROC_OWNER
else
server=/usr/lib/wine/wineserver64
# Explicit separate stock Linux server, no preload/report inherited. Its process
# is not uwph/UWP evidence; keep it alive only for this owned prefix/test.
env -u LD_PRELOAD -u UWPH_REPORT "$server" -f -p > "$run/server.log" 2>&1 &
server_pid=$!
cleanup() {
    env -u LD_PRELOAD -u UWPH_REPORT "$server" -k >/dev/null 2>&1 || true
    wait "$server_pid" || true
}
trap cleanup EXIT
sleep .2
kill -0 "$server_pid"
fi
# Fallback hint only: Debian Wine tries its installed server before WINESERVER.
# The in-process proof must therefore also inspect exec traces for server spawns.
export WINESERVER=/bin/false
export WINEDEBUG=+loaddll,+process
export WINEDLLOVERRIDES=wineboot.exe=d
status=0
for mode in control routed; do
    for pe in hello_k32 hello; do
        name=$mode-$pe
        rm -f "$run/$name.report"
        extra=(WINE_LOADER=/usr/lib/wine/wine64)
        if (( inproc )); then extra+=(LD_PRELOAD="$PWD/out/libuwph_inproc_server.so"); fi
        if [[ $mode == routed ]]; then
            extra=(WINE_LOADER="$wx/lib/wine/wine64" LD_PRELOAD="$PWD/out/libuwph_wine_shim.so" LD_DEBUG=bindings)
        fi
        rc=0
        timeout --kill-after=2s 12s strace -f -e trace=execve -o "$run/$name.exec" \
            env -u LD_PRELOAD UWPH_REPORT_PID=1 UWPH_REPORT="$run/$name.report" "${extra[@]}" \
            "$PWD/out/run_via_wine" "$root/x64/pe/tests/$pe.exe" \
            > "$run/$name.log" 2>&1 || rc=$?
        echo "$rc" > "$run/$name.exit"
        echo "$name exit=$rc ($([[ $mode == routed ]] && echo 'selected calls THROUGH W^X ntdll -> shim -> uwph' || echo 'stock Wine / launcher-only control'))"
        # Show owned output/handoff without drowning the terminal in bindings.
        sed -n '/^\[launcher\]/p; /^hello /p; /^hello.exe:/p; /^TEB self-check:/p; /^malloc:/p; /^threads:/p' "$run/$name.log"
    done
done
python3 verify_wine.py "$run" || status=$?
echo "Raw evidence retained: $run"
exit "$status"
