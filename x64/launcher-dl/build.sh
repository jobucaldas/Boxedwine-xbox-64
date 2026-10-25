#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../.."
out="$PWD/x64/launcher-dl/out"
mkdir -p "$out/obj"
cc=${CC_WIN:-x86_64-w64-mingw32-gcc}
cxx=${CXX_WIN:-x86_64-w64-mingw32-g++}
for name in adler32 crc32 inflate inffast inftrees zutil; do
  "$cc" -O2 -Ilib/zlib -c "lib/zlib/$name.c" -o "$out/obj/$name.o"
done
"$cc" -O2 -c x64/launcher-dl/third_party/zstddeclib.c -o "$out/obj/zstddeclib.o"
"$cc" -O2 -Ix64/launcher-dl/third_party/lzma -c x64/launcher-dl/third_party/lzma/LzmaDec.c -o "$out/obj/LzmaDec.o"
"$cxx" -std=c++17 -O2 -Wall -Wextra -static -static-libgcc -static-libstdc++ \
  -Ilib/zlib -Ix64/launcher-dl/third_party/lzma \
  x64/launcher-dl/src/main.cpp x64/launcher-dl/src/crypto.cpp x64/launcher-dl/src/net_winhttp.cpp \
  "$out"/obj/*.o -lwinhttp -lbcrypt -lcrypt32 -o "$out/steamdl.exe"
file "$out/steamdl.exe"
stat -c 'Windows x64 executable bytes=%s' "$out/steamdl.exe"
