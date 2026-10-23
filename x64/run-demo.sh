#!/bin/sh
# Runs x86-64 guest test programs through BoxedWine's x64 interpreter CPU.
# Usage (from repo root): x64/run-demo.sh [program [args...]]
#   default program: /bin/hello64 (static -nostdlib x86-64 hello world)
# Set BOXEDWINE_X64_STRACE=1 to log every x86-64 syscall.
set -e
REPO=$(cd "$(dirname "$0")/.." && pwd)
BW="$REPO/project/linux/Build/Release/boxedwine"
if [ ! -x "$BW" ]; then
  make -C "$REPO/project/linux" -j"$(nproc)" >/dev/null
fi
OUT="$REPO/x64/tests/out" "$REPO/x64/tests/build.sh" >/dev/null
ROOT="$REPO/x64/out/root"
mkdir -p "$ROOT/bin" "$ROOT/tmp"
cp "$REPO"/x64/tests/out/* "$ROOT/bin/"
if [ $# -eq 0 ]; then set -- /bin/hello64; fi
exec "$BW" -root "$ROOT" -w / -nozip -novideo "$@"
