#!/bin/bash
# The registry table must list exactly the GnuTLS symbols Wine 10.0's Unix
# libraries dlsym (LOAD_FUNCPTR/LOAD_FUNCPTR_OPT and literal dlsym names).
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
wine=${WINE_SOURCE:-$here/../harness/out/wine-wx-source}
files=("$wine/dlls/secur32/schannel_gnutls.c" "$wine/dlls/bcrypt/gnutls.c" "$wine/dlls/crypt32/unixlib.c")
diff <( { grep -ohE 'LOAD_FUNCPTR(_OPT)?\(\s*gnutls_[a-z0-9_]+' "${files[@]}" | sed -E 's/.*\(\s*//'
          grep -ohE '"gnutls_[a-z0-9_]+"' "${files[@]}" | tr -d '"'; } | sort -u ) \
     <(sed -n 's/^X(\(.*\))$/\1/p' "$here/wine_gnutls_symbols.h" | sort -u)
echo "wine_gnutls_symbols.h matches Wine source ($(grep -c '^X(' "$here/wine_gnutls_symbols.h") symbols)"
