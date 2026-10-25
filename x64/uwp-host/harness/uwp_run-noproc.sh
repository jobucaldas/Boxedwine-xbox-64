#!/bin/bash
# uwp_run.sh CONSTRAINTS INSTALL_DIR -- cmd...
# CONSTRAINTS: noproc,appfs,wx,nofork,net,cpu6 | stock | all
# Concession: wineserver runs OUTSIDE the jail (on Xbox it must become an in-process thread).
set -u
RD=/workspace/xbox-readiness
# Both noproc spellings hide host procfs.
if [ "${1:-}" = --noproc ]; then
  shift
  set -- noproc "$@"
fi
C=$1; INST=$2; shift 3
[ "$C" = all ] && C=noproc,appfs,nofork,net,cpu6   # wx left out: Wine PE loader uses RWX; see STATUS
export WINEPREFIX=${WINEPREFIX:-$RD/LocalState/pfx} WINEDEBUG=${WINEDEBUG:--all}
has() { [[ ",$C," == *",$1,"* ]]; }
if has nofork || has noproc; then wineserver -p 180 2>/dev/null; sleep 0.2; fi
J=(); has wx && J+=(--wx); has nofork && J+=(--nofork); has net && J+=(--net-uwp)
if has appfs; then
  J+=(--ro /usr --ro /etc --ro /lib --ro /lib64 --ro /bin --ro "$INST" --ro "$RD/bin" --rw "$WINEPREFIX" --rw /tmp --rw /dev)
fi
B=()
if has noproc; then
  # shellcheck disable=SC2206
  B=(bwrap --ro-bind /usr /usr --symlink usr/lib /lib --symlink usr/lib64 /lib64 --symlink usr/bin /bin --symlink usr/sbin /sbin
     --ro-bind /etc /etc --dev /dev --tmpfs /tmp --tmpfs /run --tmpfs /var/tmp
     --tmpfs /proc --ro-bind "$RD/fakesys" /sys
     --bind "$WINEPREFIX" "$WINEPREFIX" --ro-bind "$INST" "$INST" --ro-bind "$RD/bin" "$RD/bin"
     --chdir "$INST" --die-with-parent)
  for d in /tmp/wine-*; do [ -O "$d" ] && B+=(--bind "$d" "$d"); done
  [ -d /tmp/.X11-unix ] && B+=(--ro-bind /tmp/.X11-unix /tmp/.X11-unix)
  [ -n "${EXTRA_RW:-}" ] && B+=(--bind "$EXTRA_RW" "$EXTRA_RW")
  B+=(--)
fi
T=(); has cpu6 && T=(taskset -c 0-5)
cd "$INST"
if [ ${#J[@]} -gt 0 ]; then exec "${T[@]}" "${B[@]}" "$RD/bin/uwpjail" "${J[@]}" -- "$@"; else exec "${T[@]}" "${B[@]}" "$@"; fi
