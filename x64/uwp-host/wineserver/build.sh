#!/bin/sh
# Configure Wine 10.0 out of tree (only include/config.h is needed by the
# server) and build Wine's unmodified server/*.c into a static library.
#
#   build.sh configure   configure $WINE_BUILD only
#   build.sh [OUT_DIR]   configure if needed, then make lib
#
# WINE_SRC   Wine source tree (default /tmp/wine-src, Debian 10.0~repack)
# WINE_BUILD out-of-tree build dir (default /workspace/wine-build, not committed)
set -eu

HERE=$(cd "$(dirname "$0")" && pwd)
WINE_SRC=${WINE_SRC:-/tmp/wine-src}
WINE_BUILD=${WINE_BUILD:-/workspace/wine-build}

[ -f "$WINE_SRC/server/main.c" ] || { echo "no Wine source at $WINE_SRC" >&2; exit 1; }

# Minimal 64-bit configure: no X, no PE cross compiler, no optional libs.
# --no-create + "config.status include/config.h" generates only config.h:
# the full config.status run (makedep over the whole tree) fails on the
# Debian repack, which strips binary test resources such as
# dlls/amstream/tests/test.avi, and the server needs nothing else.
if [ ! -f "$WINE_BUILD/include/config.h" ]; then
    mkdir -p "$WINE_BUILD"
    (cd "$WINE_BUILD" && "$WINE_SRC/configure" --no-create \
        --enable-win64 --disable-tests \
        --without-x --without-freetype --without-fontconfig --without-mingw \
        --without-alsa --without-pulse --without-oss --without-cups \
        --without-dbus --without-gnutls --without-gstreamer --without-sdl \
        --without-udev --without-usb --without-v4l2 --without-vulkan \
        --without-wayland --without-opengl --without-osmesa --without-opencl \
        --without-sane --without-gphoto --without-pcap --without-pcsclite \
        --without-krb5 --without-gssapi --without-netapi --without-capi \
        --without-ffmpeg --without-unwind --without-coreaudio \
     && ./config.status include/config.h)
fi

[ "${1:-}" = configure ] && exit 0
if [ "${1:-}" = prepare ]; then
    dest=$2
    mkdir -p "$dest"
    cp "$WINE_SRC"/server/*.[ch] "$dest/"
    for patchfile in "$HERE"/patches/*.patch; do
        patch --batch -d "$dest" -p1 < "$patchfile"
    done
    cp "$HERE/patches/server_tmpdir.h" "$dest/server_tmpdir.h"
    # The Debian repack drops generated files and debian/rules reruns
    # tools/make_requests, which restarts SERVER_PROTOCOL_VERSION at 1; the
    # installed ntdll.so checks for 1. Regenerate the protocol files the same
    # way (from Wine's own protocol.def and server/*.c) so the hosted server
    # speaks the installed client's protocol. -I$(COPYDIR) picks up
    # wine/server_protocol.h ahead of $WINE_SRC/include.
    gen=$dest/.make_requests
    rm -rf "$gen"
    mkdir -p "$gen/tools" "$gen/server" "$gen/include/wine"
    cp "$WINE_SRC/tools/make_requests" "$gen/tools/"
    cp "$WINE_SRC"/server/protocol.def "$WINE_SRC"/server/*.c "$gen/server/"
    (cd "$gen" && perl tools/make_requests >/dev/null)
    grep -qx '#define SERVER_PROTOCOL_VERSION 1' "$gen/include/wine/server_protocol.h"
    mkdir -p "$dest/wine"
    cp "$gen/include/wine/server_protocol.h" "$dest/wine/server_protocol.h"
    cp "$gen/server/request_handlers.h" "$gen/server/request_trace.h" "$dest/"
    rm -rf "$gen"
    exit 0
fi
exec make -C "$HERE" WINE_SRC="$WINE_SRC" WINE_BUILD="$WINE_BUILD" ${1:+OUT="$1"} lib
