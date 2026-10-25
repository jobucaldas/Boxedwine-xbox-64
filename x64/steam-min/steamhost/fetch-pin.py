#!/usr/bin/env python3
"""Verify a historical manifest, package and DLL before publishing extraction."""
import argparse
import hashlib
import importlib.util
from pathlib import Path
import re
import shutil
import tempfile
import zipfile

ROOT = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('valve_fetch', Path(__file__).resolve().parents[1] / 'fetch-valve-client.py')
fetch = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fetch)


def fetch_legacy(pin):
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
            raise ValueError('need >=10GB before download')
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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pin', required=True, type=Path)
    args = ap.parse_args()
    pin_text = args.pin.read_text()
    legacy = 'manifest_url=https://raw.githubusercontent.com/OpenSteamClient/OpenSteamworks/3793a5db/Manifests/steam_client_win32.vdf' in pin_text.splitlines()
    pin = {}
    for line in pin_text.splitlines():
        if legacy and ('=' not in line or line.startswith(' ')):
            continue
        if line.strip() and not line.startswith('#'):
            key, value = line.split('=', 1)
            if key in pin:
                raise ValueError('duplicate pin field')
            pin[key] = value
    if pin.get('manifest_url') == 'https://raw.githubusercontent.com/OpenSteamClient/OpenSteamworks/3793a5db/Manifests/steam_client_win32.vdf':
        fetch_legacy(pin)
        return
    version = pin['manifest_version']
    if not re.fullmatch(r'[0-9]+', version):
        raise ValueError('invalid version')
    for key in ('package_sha2', 'steamclient64_sha256'):
        if not re.fullmatch(r'[0-9a-f]{64}', pin[key]):
            raise ValueError(f'invalid {key}')
    # Historical manifest is a fixed SteamTracking revision; packages stay on Valve CDN.
    if not re.fullmatch(r'https://raw.githubusercontent.com/SteamDatabase/SteamTracking/[0-9a-f]{10,40}/ClientManifest/steam_client_win64', pin['manifest_url']):
        raise ValueError('unapproved manifest URL')
    raw = fetch.get(pin['manifest_url'])
    manifest = fetch.parse_vdf(raw.decode())['win64']
    ent = manifest['bins_win64']
    base = 'https://client-update.steamstatic.com/'
    if (manifest['version'] != version or ent['sha2'] != pin['package_sha2']
            or base + ent['file'] != pin['package_url']):
        raise ValueError('pin does not match historical manifest')
    out = ROOT / 'out'
    out.mkdir(exist_ok=True)
    if shutil.disk_usage(out).free < 10 * 1024**3:
        raise ValueError('at least 10 GiB free required before download')
    cache = out / 'pin-cache'
    cache.mkdir(exist_ok=True)
    archive = fetch.download(base, ent, cache)
    target = out / f'client-{version}'
    with tempfile.TemporaryDirectory(prefix='pin-extract-', dir=out) as tmp:
        stage = Path(tmp)
        with zipfile.ZipFile(archive) as zf:
            entries = fetch.members(zf, ent['file'])
            for info, path in entries:
                dest = stage.joinpath(*path.parts)
                if info.is_dir():
                    dest.mkdir(parents=True, exist_ok=True)
                else:
                    dest.parent.mkdir(parents=True, exist_ok=True)
                    dest.write_bytes(zf.read(info))
        digest = hashlib.sha256((stage / 'steamclient64.dll').read_bytes()).hexdigest()
        if digest != pin['steamclient64_sha256']:
            raise ValueError('steamclient64.dll SHA256 mismatch')
        if target.exists():
            if target.is_symlink() or not target.is_dir():
                raise ValueError('invalid extraction destination')
            shutil.rmtree(target)
        stage.rename(target)
    (cache / f'manifest-{version}.vdf').write_bytes(raw)
    print(f'manifest_version={version}; package_sha2=verified')
    print(f'steamclient64_sha256={digest}')
    print(f'UNMODIFIED extraction -> {target}')


if __name__ == '__main__':
    main()
