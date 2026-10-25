#!/usr/bin/env bash
# run-isolated.sh CMD [ARGS...]: run one acceptance command in a private PID
# namespace (unshare -Urpf --mount-proc, unprivileged) so the unchanged B26 O2
# check `pgrep -x wineserver64 || pgrep -f '/wineserver64'` sees only this
# run's processes. Pass = that check prints nothing inside the namespace after
# CMD returns. The host-wide check is logged too, informational only.
# Logs go to the run dir: $UWPH_ISOLATED_RUN, else the staged in-process run
# (out/inproc-staged.txt), else a fresh out/isolated/run-XXXXXXXX.
# The inner re-exec keeps '/wineserver64' out of every namespace cmdline, so
# pgrep -f cannot match this wrapper itself.
set -uo pipefail
if [[ ${1:-} == --inside ]]; then
    log=$2; shift 2
    rc=0
    "$@" || rc=$?
    ns=$(pgrep -x wineserver64 || pgrep -f '/wineserver64')
    {
        echo "namespace pid=$$ (expected 1)"
        echo "command rc=$rc"
        echo "namespace pgrep (gating; pass=empty): ${ns:-<empty>}"
        echo 'namespace processes at check:'
        ps -eo pid,ppid,args
    } > "$log.ns"
    if [[ -n $ns ]]; then
        echo "run-isolated: FAIL namespace wineserver64 present: $ns" >&2
        (( rc )) || rc=1
    fi
    exit "$rc"
fi
(( $# )) || { echo 'usage: run-isolated.sh CMD [ARGS...]' >&2; exit 2; }
here=$(cd -- "$(dirname -- "$0")" && pwd)
run=${UWPH_ISOLATED_RUN:-}
if [[ -z $run && -f $here/out/inproc-staged.txt ]]; then
    IFS= read -r run < "$here/out/inproc-staged.txt"
fi
if [[ -z $run || ! -d $run ]]; then
    mkdir -p "$here/out/isolated"
    run=$(mktemp -d "$here/out/isolated/run-XXXXXXXX")
fi
tag=${UWPH_ISOLATED_TAG:-isolated} n=0
while [[ -e $run/$tag.status ]]; do n=$((n + 1)); tag=${UWPH_ISOLATED_TAG:-isolated}.$n; done
log=$run/$tag
printf '%q ' "$@" > "$log.cmd"; echo >> "$log.cmd"
rc=0
unshare -Urpf --mount-proc -- "$here/run-isolated.sh" --inside "$log" "$@" || rc=$?
{
    echo 'INFORMATIONAL ONLY (not gating): host-wide pgrep outside the namespace'
    pgrep -a -x wineserver64 || pgrep -a -f '/wineserver64'
} > "$log.host" 2>&1
ns=$(sed -n 's/^namespace pgrep (gating; pass=empty): //p' "$log.ns" 2>/dev/null)
[[ -n $ns ]] || { echo 'run-isolated: namespace check did not run' >&2; (( rc )) || rc=1; }
echo "rc=$rc namespace-pgrep=${ns:-<missing>}" > "$log.status"
echo "run-isolated: rc=$rc namespace pgrep: ${ns:-<missing log>} (logs: $log.{cmd,ns,host,status})" >&2
exit "$rc"
