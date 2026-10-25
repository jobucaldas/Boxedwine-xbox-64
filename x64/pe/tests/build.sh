#!/bin/sh
# Builds the 64-bit Windows test programs with mingw-w64 (outputs next to the sources, git-ignored).
set -e
cd "$(dirname "$0")"
x86_64-w64-mingw32-gcc -O1 -nostdlib -e entry -o hello_k32.exe hello_k32.c -lkernel32
x86_64-w64-mingw32-gcc -O1 -o hello.exe hello.c
# GUI subsystem avoids an unrelated conhost spawn; redirected stdout still carries DESK lines.
x86_64-w64-mingw32-gcc -O1 -Wall -Wextra -mwindows -o desk_probe.exe desk_probe.c -luser32 -lgdi32
