#!/usr/bin/env bash
set -euo pipefail
cd -- "$(dirname -- "$0")"
umask 077
command -v openssl >/dev/null
command -v python3 >/dev/null
test -s out/smoke.exe
mkdir -p out
stage=$(mktemp -d out/.package.XXXXXX)
trap 'rm -rf -- "$stage"' EXIT

# This key is for local development only, and never enters the payload.
# Empty PFX password is intentional for this disposable test identity.
if [[ ! -s out/test-cert.pfx || ! -s out/test-cert.cer ]]; then
    openssl req -x509 -newkey rsa:2048 -sha256 -nodes -days 365 \
        -subj '/CN=BoxedwineDev' \
        -addext 'basicConstraints=critical,CA:FALSE' \
        -addext 'keyUsage=critical,digitalSignature' \
        -addext 'extendedKeyUsage=codeSigning' \
        -keyout "$stage/key.pem" -out "$stage/cert.pem"
    openssl x509 -in "$stage/cert.pem" -outform DER -out "$stage/test-cert.cer"
    openssl pkcs12 -export -inkey "$stage/key.pem" -in "$stage/cert.pem" \
        -name BoxedwineDev -passout pass: -out "$stage/test-cert.pfx"
    mv "$stage/test-cert.cer" out/test-cert.cer
    mv "$stage/test-cert.pfx" out/test-cert.pfx
fi
openssl x509 -inform DER -in out/test-cert.cer -checkend 0 -noout
openssl pkcs12 -in out/test-cert.pfx -passin pass: -clcerts -nokeys -out "$stage/pfx-cert.pem"
openssl x509 -in "$stage/pfx-cert.pem" -outform DER -out "$stage/pfx-cert.cer"
cmp out/test-cert.cer "$stage/pfx-cert.cer"
test "$(openssl x509 -inform DER -in out/test-cert.cer -noout -subject -nameopt RFC2253)" = 'subject=CN=BoxedwineDev'

mkdir -p "$stage/payload/Assets"
cp AppxManifest.xml out/smoke.exe out/rungs.exe "$stage/payload/"
# Copy only the optional probe, never modify its source worktree.
probe=/workspace/wt/R4N/x64/native-gfx/out/probe_native.exe
if [[ -s "$probe" ]]; then cp "$probe" "$stage/payload/probe_native.exe"; fi
# Plain generated PNG tiles supply every image referenced by the manifest.
python3 - "$stage/payload" <<'PY'
import pathlib, struct, sys, zlib
import xml.etree.ElementTree as ET
root = pathlib.Path(sys.argv[1])
manifest = ET.parse(root / 'AppxManifest.xml').getroot()
ns = '{http://schemas.microsoft.com/appx/manifest/foundation/windows10}'
identity = manifest.find(ns + 'Identity')
assert identity.attrib['Publisher'] == 'CN=BoxedwineDev'
assert identity.attrib['ProcessorArchitecture'] == 'x64'
def chunk(kind, data):
    return struct.pack('>I', len(data)) + kind + data + struct.pack('>I', zlib.crc32(kind + data))
for name, size in [('StoreLogo.png', 50), ('Logo.png', 150), ('SmallLogo.png', 44)]:
    pixels = (b'\x00' + b'\x60\x20\x40\xff' * size) * size
    png = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', size, size, 8, 6, 0, 0, 0))
    png += chunk(b'IDAT', zlib.compress(pixels)) + chunk(b'IEND', b'')
    (root / 'Assets' / name).write_bytes(png)
PY
cat > "$stage/payload/PACKAGING-NOTES.txt" <<'EOF'
Loose x64 development layout. No signature or AppxBlockMap is supplied.
The certificate and private-key PFX remain outside this payload.
Use a supported Windows SDK packaging/signing workflow for a Device Portal
upload: MakeAppx pack /d payload /p BoxedwineDev.appx, then SignTool sign
/fd SHA256 /f test-cert.pfx BoxedwineDev.appx (empty test password).
Trust test-cert.cer only on your own development device as required.
rungs.exe implements a CoreApplication view; console activation is unverified.
smoke.exe remains a console build diagnostic. expandedResources is deprecated
and Store-rejected, for development sideload only. This layout
does not establish installation, activation, capability approval, or Xbox proof.
Do not ship test-cert.pfx or any private LocalState data with the app.
EOF
rm -rf out/payload
mv "$stage/payload" out/payload
printf 'Loose payload: %s/out/payload/AppxManifest.xml\nTest certificate: %s/out/test-cert.cer\nTest PFX (private; empty password): %s/out/test-cert.pfx\nBuild-only: UWP activation and on-device installation remain unverified.\n' "$PWD" "$PWD" "$PWD"
