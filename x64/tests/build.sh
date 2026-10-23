#!/bin/sh
# Builds the x86-64 guest test programs (static, non-PIE, loaded below 4GB).
set -e
cd "$(dirname "$0")"
OUT=${OUT:-out}
mkdir -p "$OUT"
gcc -nostdlib -static -no-pie -Wl,--build-id=none -o "$OUT/hello64" hello64.S
as --32 -o "$OUT/hello32.o" hello32.S && ld -m elf_i386 -o "$OUT/hello32" "$OUT/hello32.o"
as --32 -o "$OUT/exec32to64.o" exec32to64.S && ld -m elf_i386 -o "$OUT/exec32to64" "$OUT/exec32to64.o"
for c in *.c; do
  [ -f "$c" ] || continue
  gcc -O1 -static -no-pie -fno-stack-protector -o "$OUT/${c%.c}" "$c"
done
ls -l "$OUT"
