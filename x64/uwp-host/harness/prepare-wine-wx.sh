#!/usr/bin/env bash
# Build private W^X Unix + PE ntdll for the Linux harness (R4X O2, B18 O1).
#
# Source: upstream Wine 10.0 at $WINE_SRC (default /tmp/wine-src) plus the
# Debian 10.0~repack-6 patches that touch the Unix ntdll (so it matches the
# installed wine64/wineserver/PE dlls), plus the image and stub W^X patches.
# Unix ntdll.so and PE ntdll.dll are rebuilt; other DLLs in out/wine-wx link
# to the installed Debian Wine. The installed Wine and $WINE_SRC are untouched.
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
src=${WINE_SRC:-/tmp/wine-src}
deb=${WINE_DEBIAN_PATCHES:-/tmp/R4W-debian/debian/patches}
out=$PWD/out
tree=$out/wine-wx
grep -qx 'Wine version 10.0' "$src/VERSION" || { echo "prepare-wine-wx: $src is not Wine 10.0" >&2; exit 1; }
if [[ ! -d $deb ]]; then
    mkdir -p /tmp/R4W-debian && tar -xJf /tmp/R4W-wine.debian.tar.xz -C /tmp/R4W-debian
fi
stamp=$( (cat "$src/VERSION" wine-image-wx.patch wine-loader-stub-wx.patch wine-desktop-bootstrap.patch prepare-wine-wx.sh /usr/share/wine/nls/locale.nls; cat "$deb"/fixes/{temporary-directory,ntdll-398da925,ntdll-5c45391e,binutils2.44}.patch "$deb"/debianization/install-paths.patch) | sha256sum | cut -d' ' -f1)
if [[ -f $tree/.stamp && $(cat "$tree/.stamp") == "$stamp" ]]; then exit 0; fi

# Apply only the hunks of a patch that touch the listed files.
apply_files() {
    local patch=$1; shift
    python3 -I - "$patch" "$@" <<'PY' | patch -d "$out/wine-wx-source" -p1 --forward --no-backup-if-mismatch
import re, sys
text = open(sys.argv[1]).read(); want = set(sys.argv[2:])
for chunk in re.split(r'(?m)^(?=--- )', text):
    m = re.match(r'--- (\S+)\n\+\+\+ [ab]/(\S+)', chunk)
    if m and m.group(2) in want: sys.stdout.write(chunk)
PY
}

rm -rf "$out/wine-wx-source" "$out/wine-wx-build" "$tree"
mkdir -p "$out/wine-wx-source" "$out/wine-wx-build"
cp -a --reflink=auto "$src/." "$out/wine-wx-source/"
apply_files "$deb/fixes/temporary-directory.patch" dlls/ntdll/unix/server.c include/wine/server_tmpdir.h
apply_files "$deb/fixes/ntdll-398da925.patch" dlls/ntdll/unix/virtual.c
apply_files "$deb/fixes/ntdll-5c45391e.patch" dlls/ntdll/unix/virtual.c
apply_files "$deb/debianization/install-paths.patch" dlls/ntdll/unix/loader.c
patch -d "$out/wine-wx-source" -p1 --forward --no-backup-if-mismatch < wine-image-wx.patch
apply_files "$deb/fixes/binutils2.44.patch" dlls/ntdll/loader.c tools/winebuild/import.c
patch -d "$out/wine-wx-source" -p1 --forward --no-backup-if-mismatch < wine-loader-stub-wx.patch
patch -d "$out/wine-wx-source" -p1 --forward --no-backup-if-mismatch < wine-desktop-bootstrap.patch
# Debian lists the new header via a makedep.c rule; register it with makedep instead.
sed -i 's|^\twine/server.h \\$|&\n\twine/server_tmpdir.h \\|' "$out/wine-wx-source/include/Makefile.in"
# The repack drops generated files and debian/rules reruns make_requests, which
# restarts SERVER_PROTOCOL_VERSION at 1; match the installed wineserver.
rm "$out/wine-wx-source/include/wine/server_protocol.h"
(cd "$out/wine-wx-source" && perl tools/make_requests)
grep -qx '#define SERVER_PROTOCOL_VERSION 1' "$out/wine-wx-source/include/wine/server_protocol.h"

cd "$out/wine-wx-build"
# Configure without generating Makefiles: Wine's initial makedep pass scans
# unrelated test resources omitted from the Debian repack, even with tests off.
# Restrict that first pass to ntdll and its build dependencies, not a later pass.
if ! ../wine-wx-source/configure --enable-win64 --disable-tests --without-x --without-freetype --no-create > configure.log 2>&1; then
    tail -40 configure.log >&2
    exit 1
fi
python3 -I - <<'PYCONFIG'
import pathlib, re
path = pathlib.Path('config.status')
text, count = re.subn(r'(?ms)^SUBDIRS = .*?(?=^DISABLED_SUBDIRS =)',
    'SUBDIRS = include libs/musl dlls/winecrt0 dlls/ntdll dlls/oleacc dlls/stdole2.tlb '
    'dlls/stdole32.tlb tools/winebuild tools/widl tools/wrc tools/wmc tools/winegcc\n', path.read_text())
assert count == 1, 'expected one Wine SUBDIRS assignment in config.status'
path.write_text(text)
PYCONFIG
if ! ./config.status >> configure.log 2>&1; then
    tail -40 configure.log >&2
    exit 1
fi
# Debian repack omits generated NLS data; wrc needs the matching installed file.
mkdir -p nls
cp /usr/share/wine/nls/locale.nls nls/locale.nls
# Debian install dirs, so relative bin/lib/data paths match the mirrored tree.
make -j"$(nproc)" bindir=/usr/lib/wine libdir=/usr/lib/x86_64-linux-gnu/wine datadir=/usr/share/wine \
    dlls/ntdll/ntdll.so dlls/ntdll/x86_64-windows/ntdll.dll > build.log 2>&1 || { tail -20 build.log >&2; exit 1; }

# Mirror /usr/lib/wine, /usr/lib/x86_64-linux-gnu/wine and /usr/share/wine.
lib=$tree/lib/x86_64-linux-gnu/wine
mkdir -p "$tree/lib/wine" "$lib/x86_64-unix" "$tree/share"
cp /usr/lib/wine/wine64 "$tree/lib/wine/wine64"
mkdir -p "$lib/x86_64-windows"
for f in /usr/lib/x86_64-linux-gnu/wine/x86_64-windows/*; do
    [[ $(basename "$f") == ntdll.dll ]] || ln -s "$f" "$lib/x86_64-windows/"
done
cp dlls/ntdll/x86_64-windows/ntdll.dll "$lib/x86_64-windows/ntdll.dll"
for f in /usr/lib/x86_64-linux-gnu/wine/x86_64-unix/*; do
    [[ $(basename "$f") == ntdll.so ]] || ln -s "$f" "$lib/x86_64-unix/"
done
cp dlls/ntdll/ntdll.so "$lib/x86_64-unix/ntdll.so"
ln -s /usr/share/wine "$tree/share/wine"
echo "$stamp" > "$tree/.stamp"
echo "prepare-wine-wx: built $lib/x86_64-unix/ntdll.so ($(sha256sum < "$lib/x86_64-unix/ntdll.so" | cut -c1-16))"
