#!/usr/bin/env bash
# B60: pack + sign + verify the Device Portal .appx on this Linux box (no Windows PC).
#   Pack:   Microsoft's open-source MSIX SDK `makemsix pack` (built by build-makemsix.sh).
#   Sign:   osslsigncode with the kit's disposable test cert out/test-cert.pfx
#           (Publisher CN=BoxedwineDev must equal the manifest Identity Publisher).
#   Verify: linux/appx_check.py (block map hashes, content types, publisher,
#           payload equality, no private files) + osslsigncode verify.
# Input:  out/payload/ (from `make package`). payload/combo/ is NOT packed: it is
#         the Linux-hosted diagnostic tree (ELF + symlinks to Debian Wine, which
#         an .appx cannot hold) and the console app never runs it.
# Output: out/BoxedwineDev.appx (+ .sha256). Nothing is uploaded here.
set -euo pipefail
cd -- "$(dirname -- "$0")/.."
umask 077
out=out
appx=$out/BoxedwineDev.appx
test -f $out/payload/AppxManifest.xml || { echo "pack-linux: run make package first" >&2; exit 1; }
test -s $out/test-cert.pfx && test -s $out/test-cert.cer
command -v osslsigncode >/dev/null || { echo "pack-linux: osslsigncode missing (sudo apt-get install -y osslsigncode)" >&2; exit 1; }
makemsix=${MAKEMSIX:-$(command -v makemsix || true)}
[[ -n $makemsix ]] || makemsix=$(linux/build-makemsix.sh)
"$makemsix" pack -? 2>&1 | grep -q 'Creates an app package' || { echo "pack-linux: $makemsix has no pack support" >&2; exit 1; }

stage=$(mktemp -d $out/.appx.XXXXXX)
trap 'rm -rf -- "$stage"' EXIT
mkdir "$stage/payload"
# Copy the loose payload minus the Linux-only combo tree; refuse anything an appx can't hold.
tar -C $out/payload --exclude=./combo -cf - . | tar -xf - -C "$stage/payload"
if find "$stage/payload" ! -type f ! -type d | grep -q .; then
    echo "pack-linux: payload has non-regular files:" >&2; find "$stage/payload" ! -type f ! -type d >&2; exit 1
fi
if find "$stage/payload" \( -iname '*.pfx' -o -iname '*.pem' -o -iname '*.key' -o -iname '*token*' -o -name LocalState \) | grep -q .; then
    echo "pack-linux: refusing private material in payload" >&2; exit 1
fi

"$makemsix" pack -d "$stage/payload" -p "$stage/unsigned.appx" >"$stage/pack.log" 2>&1 || { cat "$stage/pack.log" >&2; exit 1; }
python3 linux/appx_check.py "$stage/unsigned.appx" --unsigned --payload "$stage/payload" --cert $out/test-cert.cer >"$stage/check-unsigned.log" \
    || { cat "$stage/check-unsigned.log" >&2; exit 1; }
# Disposable test identity: the PFX has an intentionally empty password (see pack.sh).
osslsigncode sign -pkcs12 $out/test-cert.pfx -pass '' -h sha256 \
    -in "$stage/unsigned.appx" -out "$stage/signed.appx" >"$stage/sign.log" 2>&1 || { cat "$stage/sign.log" >&2; exit 1; }
python3 linux/appx_check.py "$stage/signed.appx" --payload "$stage/payload" --cert $out/test-cert.cer
mv "$stage/signed.appx" "$appx"
(cd $out && sha256sum BoxedwineDev.appx > BoxedwineDev.appx.sha256)
echo "Signed package: $PWD/$appx ($(stat -c %s "$appx") bytes, publisher CN=BoxedwineDev)"
echo 'LINUX APPX PASS (pack+sign+verify on Linux; install/activation on the console still unverified)'
