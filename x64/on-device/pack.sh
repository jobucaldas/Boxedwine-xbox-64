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
# Hosted combo assets: keep Linux runtime separate from the native UWP entry.
combo="$stage/payload/combo"
mkdir -p "$combo"
cp out/combo-guests/hello.exe out/combo-guests/hello_k32.exe "$combo/"
cp ../uwp-host/harness/out/run_via_wine ../uwp-host/harness/out/libuwph_wine_shim.so "$combo/"
cp -a ../uwp-host/harness/out/wine-wx "$combo/"
cp combo.sh "$combo/run"
cp ../uwp-host/harness/verify_{noproc,acceptance}.py ../uwp-host/harness/surface{,-trace}.tsv "$combo/"
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

if [[ "${1:-}" == --kit ]]; then
    # Build-only bundle: no activation, signing, or probe execution occurs here.
    # Only known probe output directories are searched, never Wine prefixes,
    # LocalState, credentials, or unrelated game files.
    python3 - <<'PY'
from pathlib import Path
import hashlib
import shutil
import tempfile
import zipfile

out = Path('out')
kit = Path(tempfile.mkdtemp(prefix='.kit.', dir=out))
try:
    shutil.copytree(out / 'payload', kit / 'payload')
    for name in ('test-cert.cer', 'test-cert.pfx', 'ladder'):
        shutil.copy2(out / name, kit / name)
    shutil.copy2('README.md', kit / 'README.md')
    (kit / 'KIT-NOTES.txt').write_text(
        'BUILD-ONLY: loose layout; no console results or SDK validation.\n'
        'Use README.md for Windows SDK packaging with the existing test cert.\n'
        'test-cert.pfx is a private disposable development key: keep this kit local.\n'
        'ladder is a hosted Python diagnostic, not a console executable.\n'
        'Combo: set UWPH_COMBO_PREFIX to the prepared B21 test fixture; see README.\n'
        'Linux combo needs Debian Wine and the readiness wrapper/jail support.\n'
        'Optional probes are diagnostics, not automatically wired console rungs.\n'
        'No LocalState, tokens, Wine prefixes, or game files are included.\n')
    expected = {'test_dcomp.exe': 'f (DirectComposition diagnostic)',
                'vulkan_clear.exe': 'f (Vulkan diagnostic)',
                'probe_d3d12.exe': 'f/n2 (D3D12 diagnostic)',
                'probe_native.exe': 'n0 (desktop diagnostic; console integration pending)'}
    found = set()
    roles = {}
    for component in ('graphics', 'native-gfx'):
        for directory in sorted(Path('/workspace/wt').glob(f'*/x64/{component}/out')):
            # Flat build outputs only; keep worktree names to avoid collisions.
            sources = sorted(directory.glob('*.exe'))
            if not sources:
                continue
            for source in sources + sorted(directory.glob('*.dll')):
                if source.is_symlink() or not source.is_file() or not source.stat().st_size:
                    continue
                relative = Path('probes') / directory.parents[2].name / component / source.name
                target = kit / relative
                target.parent.mkdir(parents=True, exist_ok=True)
                shutil.copy2(source, target)
                found.add(source.name)
                roles[relative.as_posix()] = expected.get(source.name, 'f/n0 (optional probe/support DLL)')
    def role(name):
        if name in roles:
            return roles[name]
        if name.endswith('rungs.exe'):
            return 'n0-n11 (native ladder; console unverified)'
        if name.endswith('smoke.exe'):
            return 'b (build diagnostic)'
        if name.endswith('probe_native.exe'):
            return expected['probe_native.exe']
        if name == 'ladder':
            return 'a-h/n0-n11 (hosted diagnostic)'
        return 'all (installation, packaging, or instructions)'
    files = sorted(p.relative_to(kit).as_posix() for p in kit.rglob('*') if p.is_file())
    lines = [f'{name} | {role(name)}' for name in files]
    lines += ['kit-manifest.txt | all (inventory)', 'SHA256SUMS | all (integrity; excludes itself)']
    lines += [f'probes/{name} | {rung} | MISSING (optional)'
              for name, rung in expected.items() if name not in found]
    manifest = '\n'.join(lines) + '\n'
    (kit / 'kit-manifest.txt').write_text(manifest)
    files.append('kit-manifest.txt')
    (kit / 'SHA256SUMS').write_text(''.join(
        f'{hashlib.sha256((kit / name).read_bytes()).hexdigest()}  {name}\n'
        for name in sorted(files)))
    archive = out / '.kit.zip'
    with zipfile.ZipFile(archive, 'w', zipfile.ZIP_DEFLATED) as z:
        for name in sorted(files + ['SHA256SUMS']):
            z.write(kit / name, name)
    # Check inventory against the actual archive before publishing it.
    with zipfile.ZipFile(archive) as z:
        assert set(z.namelist()) == set(files + ['SHA256SUMS'])
    shutil.rmtree(out / 'kit', ignore_errors=True)
    kit.rename(out / 'kit')
    archive.replace(out / 'kit.zip')
    # Sidecar verifies the archive and every extracted member, including the
    # kit's own checksum manifest, from the package output directory.
    sidecar_files = [out / 'kit.zip'] + sorted(
        p for p in (out / 'kit').rglob('*') if p.is_file())
    (out / 'SHA256SUMS').write_text(''.join(
        f'{hashlib.sha256(p.read_bytes()).hexdigest()}  {p.relative_to(out).as_posix()}\n'
        for p in sidecar_files))
    Path('kit-manifest.txt').write_text(manifest)
    print((out / 'kit.zip').resolve(), flush=True)
finally:
    if kit.exists():
        shutil.rmtree(kit)
PY
fi
