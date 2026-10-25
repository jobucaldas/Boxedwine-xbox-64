#!/usr/bin/env bash
# Build-only bundle verification; no Wine or device activation.
set -euo pipefail
cd -- "$(dirname -- "$0")"
kit=${1:-out/kit}
command -v xmllint >/dev/null
xmllint --nonet --noout "$kit/payload/AppxManifest.xml"
python3 -I - "$kit" <<'PY'
import sys
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

kit = Path(sys.argv[1])
errors = []
listed = set()
for number, line in enumerate((kit / 'kit-manifest.txt').read_text().splitlines(), 1):
    fields = line.split(' | ')
    if len(fields) < 2:
        errors.append(f'invalid inventory line {number}')
        continue
    name = fields[0]
    path = PurePosixPath(name)
    if path.is_absolute() or '..' in path.parts or path.as_posix() != name:
        errors.append(f'invalid inventory path: {name}')
        continue
    if any(field == 'MISSING (optional)' for field in fields[2:]):
        continue  # Existing inventory convention: explicitly absent optional probes.
    if name in listed:
        errors.append(f'duplicate inventory path: {name}')
    listed.add(name)
actual = set()
for path in kit.rglob('*'):
    name = path.relative_to(kit).as_posix()
    if path.is_file():
        actual.add(name)
    elif not path.is_dir():
        errors.append(f'unsupported package entry: {name}')
for name in sorted(listed - actual):
    errors.append(f'missing listed file: {name}')
for name in sorted(actual - listed):
    errors.append(f'unlisted shipped file: {name}')
ns = '{http://schemas.microsoft.com/appx/manifest/foundation/windows10}'
root = ET.parse(kit / 'payload/AppxManifest.xml').getroot()
identity = root.find(ns + 'Identity')
families = root.findall(ns + 'Dependencies/' + ns + 'TargetDeviceFamily')
if identity is None or not identity.get('Name') or not identity.get('Version'):
    errors.append('missing Identity Name/Version')
else:
    print('Identity: ' + ' '.join(f'{k}={v}' for k, v in identity.attrib.items()))
    print('Version: ' + identity.attrib['Version'])
if not families:
    errors.append('missing TargetDeviceFamily')
for family in families:
    print('TargetDeviceFamily: ' + ' '.join(f'{k}={v}' for k, v in family.attrib.items()))
for error in errors:
    print('verify-package: ' + error, file=sys.stderr)
if errors:
    sys.exit(1)
print(f'verify-package: PASS ({len(actual)} files; XML well-formed; build-only)')
PY
