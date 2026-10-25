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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--pin', required=True, type=Path)
    args = ap.parse_args()
    pin = {}
    for line in args.pin.read_text().splitlines():
        if line.strip() and not line.startswith('#'):
            key, value = line.split('=', 1)
            if key in pin:
                raise ValueError('duplicate pin field')
            pin[key] = value
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
