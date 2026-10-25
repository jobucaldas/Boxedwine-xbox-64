#!/usr/bin/env python3
"""Fetch Valve's minimal Windows Steam client tree from Valve's own manifest.

GETs Valve's client manifest (--manifest steam_client_win64, default, or
steam_cmd_win32 for steamcmd), downloads the zips for the requested --set
from the same host, checks each against the manifest's
"sha2", and unzips them unmodified into --out (a prefix's
drive_c/Program Files (x86)/Steam). CEF/webhelper packages are refused
(exit 2) so steamwebhelper can't be installed. Writes
--out/../steam-min-install.json with the version, the files and the sha256
of every extracted file.
"""
import argparse
import fnmatch
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import sys
import urllib.request
import zipfile

HOSTS = [
    'https://client-update.steamstatic.com/',
    'https://cdn.steamstatic.com/client/',
]
# manifest name -> (root block, default sets)
MANIFESTS = {
    'steam_client_win64': ('win64', ['bins_win64']),
    'steam_cmd_win32': ('win32', ['steamcmd_win32', 'steamcmd_bins_win32',
                                  'steamcmd_public_all']),
}
REFUSED = ['bins_cef_win64', 'bins_webhelpers_win64', 'steamui_websrc_*']
REFUSED_MEMBERS = ['steamwebhelper*', 'libcef*']
UA = 'steam-min-fetch/1 (Valve client manifest, unmodified files)'
HERE = Path(__file__).resolve().parent


def refused(name, patterns):
    return any(fnmatch.fnmatch(name.lower(), p) for p in patterns)


def get(url):
    req = urllib.request.Request(url, headers={'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=60) as r:
        if r.status != 200:
            raise OSError(f'HTTP {r.status}')
        return r.read()


def parse_vdf(text):
    """Text KeyValues: "key" "value" pairs and "key" { ... } blocks."""
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c.isspace():
            i += 1
        elif c in '{}':
            toks.append(c)
            i += 1
        elif text.startswith('//', i):
            i = text.find('\n', i)
            i = n if i < 0 else i
        elif c == '"':
            j = i + 1
            s = []
            while j < n and text[j] != '"':
                if text[j] == '\\' and j + 1 < n:
                    j += 1
                s.append(text[j])
                j += 1
            if j >= n:
                raise ValueError('unterminated string in manifest')
            toks.append(('s', ''.join(s)))
            i = j + 1
        else:
            raise ValueError(f'unexpected {c!r} in manifest')
    pos = 0

    def block():
        nonlocal pos
        d = {}
        while pos < len(toks):
            t = toks[pos]
            if t == '}':
                pos += 1
                return d
            if t == '{' or pos + 1 >= len(toks):
                raise ValueError('malformed manifest')
            key = t[1]
            nxt = toks[pos + 1]
            pos += 2
            if nxt == '{':
                d[key] = block()
            elif nxt == '}':
                raise ValueError('malformed manifest')
            else:
                d[key] = nxt[1]
        return d

    return block()


def fetch_manifest(name):
    key = MANIFESTS[name][0]
    errs = []
    for host in HOSTS:
        url = host + name
        try:
            data = get(url)
            vdf = parse_vdf(data.decode('utf-8'))
            root = vdf.get(key)
            if not isinstance(root, dict) or 'version' not in root:
                raise ValueError(f'no "{key}" version block')
            return host, root, data
        except (OSError, ValueError) as e:
            errs.append(f'{url}: {e}')
    sys.exit('manifest fetch failed:\n  ' + '\n  '.join(errs))


def download(base, ent, cache):
    name, sha2, size = ent['file'], ent['sha2'].lower(), int(ent['size'])
    if '/' in name or '\\' in name or name.startswith('.'):
        sys.exit(f'refusing odd file name {name!r}')
    path = cache / name
    if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == sha2:
        print(f'cached {name}')
        return path
    print(f'GET {base}{name} ({size} bytes)')
    data = get(base + name)
    got = hashlib.sha256(data).hexdigest()
    if len(data) != size or got != sha2:
        sys.exit(f'{name}: sha2/size mismatch (got {got}, {len(data)} bytes)')
    tmp = path.with_suffix('.part')
    tmp.write_bytes(data)
    tmp.replace(path)
    return path


def members(zf, name):
    out = []
    for info in zf.infolist():
        p = PurePosixPath(info.filename.replace('\\', '/'))
        if p.is_absolute() or '..' in p.parts or ':' in info.filename:
            sys.exit(f'{name}: unsafe path {info.filename!r}')
        if any(refused(part, REFUSED_MEMBERS) for part in p.parts):
            print(f'REFUSED: {name} contains {info.filename} (CEF/webhelper)',
                  file=sys.stderr)
            sys.exit(2)
        out.append((info, p))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', required=True, type=Path,
                    help='target Steam dir (drive_c/Program Files (x86)/Steam)')
    ap.add_argument('--manifest', choices=sorted(MANIFESTS),
                    default='steam_client_win64',
                    help='Valve manifest to read (default steam_client_win64)')
    ap.add_argument('--set', action='append', default=[],
                    help='manifest package(s), repeatable or comma-separated '
                         '(default bins_win64, or steamcmd_win32,'
                         'steamcmd_bins_win32,steamcmd_public_all for '
                         'steam_cmd_win32; e.g. public_all, strings_en_all, '
                         'resources_all, steam_win64)')
    ap.add_argument('--cache', type=Path, default=HERE / 'out' / 'cache')
    a = ap.parse_args()

    sets = [s.strip() for v in (a.set or MANIFESTS[a.manifest][1])
            for s in v.split(',')
            if s.strip()]
    bad = [s for s in sets if refused(s, REFUSED)]
    if bad:
        print(f'REFUSED: {", ".join(bad)} (CEF/webhelper/web UI is never '
              'installed by steam-min)', file=sys.stderr)
        return 2

    base, root, raw = fetch_manifest(a.manifest)
    print(f'manifest {base} version {root["version"]}')
    ents = []
    for s in dict.fromkeys(sets):
        ent = root.get(s)
        if not isinstance(ent, dict) or 'file' not in ent or 'sha2' not in ent:
            sys.exit(f'set {s!r} not in manifest (or has no file/sha2)')
        ents.append((s, ent))

    a.cache.mkdir(parents=True, exist_ok=True)
    (a.cache / f'{a.manifest}.{root["version"]}.vdf').write_bytes(raw)
    zips = [(s, ent, download(base, ent, a.cache)) for s, ent in ents]

    out = a.out.resolve()
    out.mkdir(parents=True, exist_ok=True)
    files = {}
    pkgs = []
    for s, ent, path in zips:
        with zipfile.ZipFile(path) as zf:
            for info, p in members(zf, ent['file']):
                dest = out.joinpath(*p.parts)
                if info.is_dir():
                    dest.mkdir(parents=True, exist_ok=True)
                    continue
                dest.parent.mkdir(parents=True, exist_ok=True)
                data = zf.read(info)
                dest.write_bytes(data)
                files[p.as_posix()] = {
                    'sha256': hashlib.sha256(data).hexdigest(),
                    'size': len(data), 'set': s}
        pkgs.append({'set': s, 'file': ent['file'], 'sha2': ent['sha2'],
                     'size': int(ent['size'])})
        print(f'extracted {ent["file"]}')

    rec = {'manifest': base + a.manifest,
           'version': root['version'], 'packages': pkgs,
           'files': dict(sorted(files.items()))}
    meta = out.parent / 'steam-min-install.json'
    meta.write_text(json.dumps(rec, indent=1) + '\n')
    print(f'{len(files)} files -> {out}; wrote {meta}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
