#!/usr/bin/env bash
# Read-only, anonymous metadata watcher. No client packages are downloaded.
set -uo pipefail
exec python3 - "$(dirname -- "${BASH_SOURCE[0]}")/PIN.txt" <<'PY'
import json
import re
import sys
import urllib.error
import urllib.request

API = 'https://api.github.com/repos/OpenSteamClient/OpenSteamworks'
RAW = 'https://raw.githubusercontent.com/OpenSteamClient/OpenSteamworks'
CDN = 'https://client-update.steamstatic.com/'


def request(url, method='GET', missing=False):
    req = urllib.request.Request(url, method=method,
                                 headers={'User-Agent': 'osw-watch/1.0'})
    try:
        with urllib.request.urlopen(req, timeout=30) as response:
            return response.status, response.read() if method == 'GET' else b''
    except urllib.error.HTTPError as error:
        if missing and error.code == 404:
            return 404, b''
        raise


def get_json(url):
    return json.loads(request(url)[1])


def version(manifest):
    match = re.search(r'"version"\s*"(\d+)"', manifest)
    if not match:
        raise ValueError('manifest has no version')
    return match[1]


try:
    # Five GitHub requests maximum, including the optional win64 404.
    head = get_json(API + '/commits/HEAD')['sha']
    print('osw_head=' + head, flush=True)
    manifest_path = 'Manifests/steam_client_win64.vdf'
    status, body = request(f'{RAW}/{head}/{manifest_path}', missing=True)
    if status == 404:
        manifest_path = 'Manifests/steam_client_win32.vdf'
        body = request(f'{RAW}/{head}/{manifest_path}')[1]
    manifest = body.decode('utf-8-sig')
    build = version(manifest)
    print(f'osw_tables_build={build} (from {manifest_path})', flush=True)
    changes = get_json(API + f'/commits?sha={head}&path=OpenSteamworks/Generated&per_page=1')
    change = changes[0]
    print(f'generated_last_change={change["sha"]} {change["commit"]["committer"]["date"]}', flush=True)
    # Select the actual bins archive in that exact manifest, never CEF/webhelpers.
    package = re.search(r'"bins_(win64|win32)"\s*\{([^{}]*)\}', manifest)
    if not package:
        raise ValueError('manifest has no bins package')
    archive = re.search(r'"file"\s*"([A-Za-z0-9_.-]+)"', package[2])
    if not archive:
        raise ValueError('bins package has no valid file')
    filename = archive[1]
    status, _ = request(CDN + filename, method='HEAD', missing=True)
    if status not in (200, 404):
        raise ValueError(f'unexpected bins HTTP status: {status}')
    print(f'cdn_bins={status} {filename}', flush=True)
    with open(sys.argv[1], encoding='utf-8') as pin_file:
        pin = re.search(r'^manifest_version=(\d+)$', pin_file.read(), re.M)
    if not pin:
        raise ValueError('PIN.txt has no manifest_version')
    print('current_pin=' + pin[1], flush=True)
    current = version(request(CDN + 'steam_client_win64')[1].decode('utf-8-sig'))
    print('valve_current=' + current, flush=True)
    verdict = 'none' if status == 404 else ('match-current' if build == current else 'match-old-only')
    print('verdict=' + verdict)
except Exception as error:
    print(f'osw-watch: metadata/network failure: {error}', file=sys.stderr)
    sys.exit(2)
PY
