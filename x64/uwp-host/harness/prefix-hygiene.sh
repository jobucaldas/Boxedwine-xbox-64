# Shared by owned harness scripts; source before creating a per-run directory.
refuse_dereferenced_prefix() {
    local bad
    bad=$(find -P "$1" -type d -path '*/dosdevices/z:' -print -quit)
    [[ -z $bad ]] || { echo "Refusing dereferenced dosdevices/z: directory: $bad" >&2; return 1; }
}
run= server_pid=
cleanup_run() {
    local rc=$?
    trap - EXIT
    if [[ -n $server_pid ]]; then
        env -u LD_PRELOAD -u UWPH_REPORT /usr/lib/wine/wineserver64 -k >/dev/null 2>&1 || true
        wait "$server_pid" || true
    fi
    if [[ -n $run && ${KEEP_RUN:-0} != 1 ]]; then
        if [[ -d $run/prefix ]]; then chmod -R u+w -- "$run/prefix"; fi
        rm -rf -- "$run/prefix"
        # Keep logs, reports and exits even on failure, without retaining run-*.
        mv -- "$run" "${run%/run-*}/evidence-${run##*/run-}"
    fi
    exit "$rc"
}
trap cleanup_run EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
trap 'exit 129' HUP
