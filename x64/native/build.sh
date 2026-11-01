#!/bin/sh
# Builds the approach-2 native passthrough runner (Linux x86-64 host only).
set -e
cd "$(dirname "$0")"
mkdir -p out
gcc -O2 -Wall -fno-stack-protector -o out/x64native x64native.c
