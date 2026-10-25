#!/bin/bash
# B44 O2 spike: build the GnuTLS provider uwph ships (decision (a) in
# docs/tls-on-xbox.md). Pinned upstream releases, sha256-checked; nothing
# is vendored in the tree.
#
#   build-gnutls.sh linux  -> out/linux/lib/libgnutls.so.30 (Linux stand-in:
#                             one self-contained object, nettle/hogweed/gmp/
#                             libtasn1 linked in, no host TLS libraries needed)
#   build-gnutls.sh win    -> out/win/lib/libgnutls.a (+ nettle/hogweed/gmp),
#                             x86_64-w64-mingw32 static archives that the
#                             Win32 backend links and registers as
#                             "libgnutls.so.30" in uwph's dl registry.
#
# Licences: GnuTLS/libtasn1 LGPL-2.1+, nettle/GMP LGPL-3+ or GPL-2+ (used
# under GPL-2+). The GPL-3 GnuTLS tools are not built.
set -euo pipefail
flavor=${1:?usage: build-gnutls.sh linux|win}
here=$(cd "$(dirname "$0")" && pwd)
out=$here/out
dl=${UWPH_TLS_DL:-$out/dl}
jobs=$(nproc 2>/dev/null || echo 4)

GMP=gmp-6.3.0.tar.xz
GMP_SHA=a3c2b80201b89e68616f4ad30bc66aee4927c3ce50e33929ca819d5c43538898
NETTLE=nettle-3.10.1.tar.gz
NETTLE_SHA=b0fcdd7fc0cdea6e80dcf1dd85ba794af0d5b4a57e26397eee3bc193272d9132
GNUTLS=gnutls-3.8.9.tar.xz
GNUTLS_SHA=69e113d802d1670c4d5ac1b99040b1f2d5c7c05daec5003813c049b5184820ed

fetch() { # file sha url
    mkdir -p "$dl"
    if [[ ! -f $dl/$1 ]]; then curl -sSfL -o "$dl/$1.part" "$3"; mv "$dl/$1.part" "$dl/$1"; fi
    echo "$2  $dl/$1" | sha256sum -c --quiet -
}
fetch $GMP $GMP_SHA https://ftp.gnu.org/gnu/gmp/$GMP
fetch $NETTLE $NETTLE_SHA https://ftp.gnu.org/gnu/nettle/$NETTLE
fetch $GNUTLS $GNUTLS_SHA https://www.gnupg.org/ftp/gcrypt/gnutls/v3.8/$GNUTLS

case $flavor in
linux) host=; cc=${CC_LINUX:-gcc}; pic=--with-pic; extra_cflags="-O2 -fPIC" ;;
win)   host=--host=x86_64-w64-mingw32; cc=x86_64-w64-mingw32-gcc; pic=; extra_cflags="-O2" ;;
*) echo "unknown flavor $flavor" >&2; exit 2 ;;
esac
prefix=$out/$flavor
src=$out/$flavor-src
rm -rf "$src"; mkdir -p "$src" "$prefix"
for a in $GMP $NETTLE $GNUTLS; do tar -C "$src" -xf "$dl/$a"; done
export PKG_CONFIG_LIBDIR=$prefix/lib/pkgconfig PKG_CONFIG_PATH=
export CC=$cc CFLAGS="$extra_cflags" CPPFLAGS="-I$prefix/include" LDFLAGS="-L$prefix/lib"

(cd "$src/gmp-6.3.0" && ./configure $host --prefix="$prefix" --disable-shared --enable-static $pic \
    && make -j"$jobs" && make install) > "$out/$flavor-gmp.log" 2>&1
(cd "$src/nettle-3.10.1" && ./configure $host --prefix="$prefix" --libdir="$prefix/lib" \
    --disable-shared --enable-static --disable-documentation --disable-openssl \
    && make -j"$jobs" && make install) > "$out/$flavor-nettle.log" 2>&1

# Wine verifies certificates itself (crypt32 chain engine); the provider only
# needs the TLS engine + X.509/PKCS primitives, so no trust store, PKCS#11,
# IDN, compression, TPM or tools.
common=(--prefix="$prefix" --libdir="$prefix/lib" --with-included-libtasn1 --with-included-unistring
    --without-p11-kit --without-idn --without-zlib --without-brotli --without-zstd
    --without-tpm --without-tpm2 --disable-tools --disable-doc --disable-manpages
    --disable-tests --disable-cxx --disable-nls --disable-libdane --disable-guile
    --with-default-trust-store-file= --with-default-trust-store-dir=
    --disable-full-test-suite)
if [[ $flavor == linux ]]; then
    common+=(--enable-shared --disable-static)
else
    common+=(--disable-shared --enable-static)
fi
(cd "$src/gnutls-3.8.9" && \
    NETTLE_CFLAGS="-I$prefix/include" NETTLE_LIBS="$prefix/lib/libnettle.a" \
    HOGWEED_CFLAGS="-I$prefix/include" HOGWEED_LIBS="$prefix/lib/libhogweed.a $prefix/lib/libgmp.a" \
    GMP_CFLAGS="-I$prefix/include" GMP_LIBS="$prefix/lib/libgmp.a" \
    ./configure $host "${common[@]}" && make -j"$jobs" && make install) > "$out/$flavor-gnutls.log" 2>&1
echo "built $flavor provider in $prefix"
