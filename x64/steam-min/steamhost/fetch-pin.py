#!/usr/bin/env python3
"""Fetch only the pinned Valve bins package; extract three unmodified DLLs."""
import argparse
import hashlib
from pathlib import Path
import sys
import shutil
import zipfile
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from importlib import import_module
fetch = import_module('fetch-valve-client')
p = argparse.ArgumentParser(description=__doc__)
p.add_argument('--pin', required=True, type=Path)
a = p.parse_args()
pin = dict(line.split('=', 1) for line in a.pin.read_text().splitlines()
           if '=' in line and not line.startswith('#') and not line.startswith(' '))
assert pin['manifest_version'] == '1745623383', 'only the second pin is supported'
assert pin['manifest_url'] == 'https://raw.githubusercontent.com/OpenSteamClient/OpenSteamworks/3793a5db/Manifests/steam_client_win32.vdf'
root = fetch.parse_vdf(fetch.get(pin['manifest_url']).decode('utf-8-sig'))['win32']
ent = root['bins_win32']
assert root['version'] == pin['manifest_version']
assert ent['file'] == pin['package_file'] and ent['file'] == 'bins_win32.zip.f5eaa61d1767c375d0c8817858df17cb309fe5f1'
assert ent['sha2'] == pin['package_sha2'] and int(ent['size']) == int(pin['package_size'])
here = Path(__file__).resolve().parent
cache = here / 'out/cache'
cache.mkdir(parents=True, exist_ok=True)
if not (cache / ent['file']).is_file():
    if shutil.disk_usage('/').free < 10_000_000_000:
        sys.exit('need >=10GB before download')
archive = fetch.download('https://client-update.steamstatic.com/', ent, cache)
with zipfile.ZipFile(archive) as z:
    data = {n: z.read(n) for n in ('steamclient64.dll', 'tier0_s64.dll', 'vstdlib_s64.dll')}
assert hashlib.sha256(data['steamclient64.dll']).hexdigest() == pin['steamclient64_sha256']
out = here / ('out/client-' + pin['manifest_version'])
out.mkdir(parents=True, exist_ok=True)
for name, content in data.items():
    (out / name).write_bytes(content)
    print(f'{name} sha256={hashlib.sha256(content).hexdigest()}')
print(f'package_sha2=verified client_build={pin["manifest_version"]} out={out}')
