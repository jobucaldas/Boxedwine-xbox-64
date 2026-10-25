#!/usr/bin/env python3
"""Minimal Xbox Device Portal (WDP) REST client for the B60 remote flow.

Only documented WDP endpoints (learn.microsoft.com/windows/uwp/debug-test-perf/
device-portal-api-core and .../xbox-apps/wdp-xboxsettings-api):
  GET  /api/os/info, /api/os/devicefamily
  POST /api/app/packagemanager/package?package=NAME   (multipart upload)
  GET  /api/app/packagemanager/state                  (204 running, 200 result)
  GET  /api/app/packagemanager/packages
  DELETE /api/app/packagemanager/package?package=FULLNAME
  POST/DELETE /api/taskmanager/app?appid=B64&package=B64
  GET  /api/filesystem/apps/files|file?knownfolderid=LocalAppData&...
  GET/PUT /ext/settings[/NAME]
  GET  /ext/screenshot?download=true
  POST /api/control/restart
Environment: DP_URL (https://<console-ip>:11443), DP_USER, DP_PASS.
Credentials are sent only as an HTTP Basic header and never printed or logged.
TLS: the console's Device Portal cert is self-signed, so verification is off;
set DP_TLS_SHA256 to pin the cert fingerprint printed by `info`.
"""
import argparse, base64, hashlib, http.client, json, os, re, ssl, sys, time, uuid
import xml.etree.ElementTree as ET
from pathlib import Path
from urllib.parse import urlencode, urlsplit

HERE = Path(__file__).resolve().parent
DEFAULT_MANIFEST = HERE.parent / 'out' / 'payload' / 'AppxManifest.xml'
MF = '{http://schemas.microsoft.com/appx/manifest/foundation/windows10}'

class DPError(Exception):
    pass

def b64(s):
    return base64.b64encode(s.encode()).decode()

class DevicePortal:
    def __init__(self, url, user, password, pin=None, timeout=60):
        u = urlsplit(url)
        if u.scheme not in ('https', 'http') or not u.hostname:
            raise DPError('DP_URL must look like https://<console-ip>:11443')
        self.scheme, self.host, self.port = u.scheme, u.hostname, u.port or (443 if u.scheme == 'https' else 80)
        self._auth = 'Basic ' + base64.b64encode(f'{user}:{password}'.encode()).decode()
        self._secrets = [s for s in (user, password, self._auth) if s]
        self.pin = pin.replace(':', '').lower() if pin else None
        self.timeout = timeout
        self.cookies = {}
        self.fingerprint = None

    def redact(self, text):
        for s in self._secrets:
            text = text.replace(s, '***')
        return text

    def _conn(self):
        if self.scheme == 'http':
            return http.client.HTTPConnection(self.host, self.port, timeout=self.timeout)
        ctx = ssl.create_default_context()
        ctx.check_hostname = False
        ctx.verify_mode = ssl.CERT_NONE  # console Device Portal cert is self-signed
        return http.client.HTTPSConnection(self.host, self.port, timeout=self.timeout, context=ctx)

    def request(self, method, path, query=None, body=None, headers=None, ok=(200,)):
        if query:
            path += '?' + urlencode(query)
        h = {'Authorization': self._auth, 'Accept': '*/*'}
        if self.cookies:
            h['Cookie'] = '; '.join(f'{k}={v}' for k, v in self.cookies.items())
        if method != 'GET' and 'CSRF-Token' in self.cookies:
            h['X-CSRF-Token'] = self.cookies['CSRF-Token']   # WDP CSRF double-submit
        h.update(headers or {})
        c = self._conn()
        try:
            c.connect()
            if self.scheme == 'https':
                # Check the pin BEFORE any request (and its Authorization header) is sent.
                der = c.sock.getpeercert(binary_form=True)
                self.fingerprint = hashlib.sha256(der).hexdigest()
                if self.pin and self.pin != self.fingerprint:
                    raise DPError(f'TLS fingerprint {self.fingerprint} does not match DP_TLS_SHA256; nothing was sent')
            c.request(method, path, body=body, headers=h)
            r = c.getresponse()
            data = r.read()
            for k, v in r.getheaders():
                if k.lower() == 'set-cookie':
                    name, _, rest = v.partition('=')
                    self.cookies[name.strip()] = rest.split(';', 1)[0]
        except (OSError, http.client.HTTPException) as e:
            raise DPError(self.redact(f'{method} {path.split("?")[0]}: {e}')) from None
        finally:
            c.close()
        if r.status == 401:
            raise DPError('401 Unauthorized: check DP_USER/DP_PASS (Dev Home > Remote Access Settings)')
        if r.status not in ok:
            raise DPError(self.redact(f'{method} {path.split("?")[0]} -> HTTP {r.status}: {data[:400].decode("utf-8", "replace")}'))
        return r.status, data

    def json(self, method, path, query=None, **kw):
        _, data = self.request(method, path, query, **kw)
        return json.loads(data or b'{}')

    def prime(self):
        """GET first: verifies auth and collects the CSRF-Token cookie for writes."""
        return self.json('GET', '/api/os/info')

    # -- deployment --
    def install(self, appx, extra=(), wait=600, poll=2.0, log=print):
        files = [Path(appx)] + [Path(p) for p in extra]
        boundary = 'b60-' + uuid.uuid4().hex
        parts, size = [], 0
        for f in files:
            head = (f'--{boundary}\r\nContent-Disposition: form-data; name="{f.name}"; filename="{f.name}"\r\n'
                    f'Content-Type: application/octet-stream\r\n\r\n').encode()
            parts.append((head, f))
            size += len(head) + f.stat().st_size + 2
        tail = f'--{boundary}--\r\n'.encode()
        size += len(tail)
        def body():
            for head, f in parts:
                yield head
                with f.open('rb') as fh:
                    while chunk := fh.read(1 << 20):
                        yield chunk
                yield b'\r\n'
            yield tail
        self.prime()
        log(f'upload {files[0].name} ({files[0].stat().st_size} bytes) + {len(files) - 1} extra file(s)')
        self.request('POST', '/api/app/packagemanager/package', {'package': files[0].name}, body=body(),
                     headers={'Content-Type': f'multipart/form-data; boundary={boundary}',
                              'Content-Length': str(size)}, ok=(200, 202))
        deadline, misses = time.time() + wait, 0
        while time.time() < deadline:
            status, data = self.request('GET', '/api/app/packagemanager/state', ok=(200, 204, 404))
            if status == 200:
                result = json.loads(data or b'{}')
                if result.get('Success', result.get('Code', 1) == 0):
                    log('install OK')
                    return result
                raise DPError(f'install failed: Code={result.get("Code")} CodeText={result.get("CodeText")!r} '
                              f'Reason={result.get("Reason")!r}')
            if status == 404:
                misses += 1
                if misses > 10:
                    raise DPError('install state: 404 (no installation found) 10 times in a row')
            time.sleep(poll)
        raise DPError(f'install still running after {wait}s')

    def packages(self):
        return self.json('GET', '/api/app/packagemanager/packages').get('InstalledPackages', [])

    def find(self, identity_name):
        hits = [p for p in self.packages() if p.get('PackageFullName', '').startswith(identity_name + '_')
                or p.get('Name') == identity_name]
        if not hits:
            raise DPError(f'package {identity_name} is not installed (run install first)')
        return hits[0]

    def uninstall(self, full):
        self.prime()
        self.request('DELETE', '/api/app/packagemanager/package', {'package': full})

    def launch(self, full, appid):
        self.prime()
        self.request('POST', '/api/taskmanager/app', {'appid': b64(appid), 'package': b64(full)})

    def stop(self, full):
        self.prime()
        self.request('DELETE', '/api/taskmanager/app', {'package': b64(full), 'forcestop': 'yes'})

    # -- app files (LocalAppData of the sideloaded package) --
    def files(self, full, path):
        return self.json('GET', '/api/filesystem/apps/files',
                         {'knownfolderid': 'LocalAppData', 'packagefullname': full, 'path': path}).get('Items', [])

    def pull(self, full, path, name, dest):
        _, data = self.request('GET', '/api/filesystem/apps/file',
                               {'knownfolderid': 'LocalAppData', 'packagefullname': full,
                                'path': path, 'filename': name})
        dest.parent.mkdir(parents=True, exist_ok=True)
        dest.write_bytes(data)
        return len(data)

def manifest_identity(path):
    root = ET.parse(path).getroot()
    ident = root.find(MF + 'Identity').get('Name')
    apps = [a.get('Id') for a in root.find(MF + 'Applications')]
    return ident, apps

def main(argv=None):
    ap = argparse.ArgumentParser(prog='deploy-devportal.sh', description=__doc__.split('\n')[0])
    ap.add_argument('--manifest', default=str(DEFAULT_MANIFEST), help='AppxManifest.xml for identity/app ids')
    ap.add_argument('--timeout', type=float, default=120)
    sub = ap.add_subparsers(dest='cmd', required=True)
    sub.add_parser('info', help='OS info + device family + TLS fingerprint (auth check)')
    p = sub.add_parser('install', help='upload + install the signed .appx, poll state')
    p.add_argument('appx', nargs='?', default=str(HERE.parent / 'out' / 'BoxedwineDev.appx'))
    p.add_argument('--extra', action='append', default=[], help='dependency package or .cer (not needed on Xbox)')
    p.add_argument('--wait', type=float, default=900)
    sub.add_parser('list', help='installed packages (this app first)')
    p = sub.add_parser('launch', help='start an app tile (App or BigFile)'); p.add_argument('app', nargs='?', default=None)
    sub.add_parser('stop', help='stop the app (forcestop)')
    p = sub.add_parser('files', help='list app data files'); p.add_argument('--path', default='\\LocalState')
    p = sub.add_parser('pull', help='download app data files')
    p.add_argument('names', nargs='*', default=['ladder-native.log'])
    p.add_argument('--path', default='\\LocalState'); p.add_argument('--out', default='devportal-pull')
    p = sub.add_parser('pull-logs', help='download every *.log/*.txt/*.json in LocalState into DIR')
    p.add_argument('out', nargs='?', default='devportal-pull'); p.add_argument('--path', default='\\LocalState')
    sub.add_parser('settings', help='list /ext/settings (look for the UWP-as-Game default)')
    p = sub.add_parser('set-setting', help='PUT /ext/settings/NAME {"Value": VALUE}')
    p.add_argument('name'); p.add_argument('value')
    p = sub.add_parser('screenshot', help='save a console screenshot PNG'); p.add_argument('--out', default='devportal-screenshot.png')
    p = sub.add_parser('uninstall', help='remove this app and its LocalState (confirm with: uninstall yes)')
    p.add_argument('confirm', nargs='?', default='')
    p = sub.add_parser('reboot', help='restart the console (confirm with: reboot now)')
    p.add_argument('confirm', nargs='?', default='')
    a = ap.parse_args(argv)

    url, user, pw = os.environ.get('DP_URL'), os.environ.get('DP_USER'), os.environ.get('DP_PASS')
    if not url or user is None or pw is None:
        print('deploy-devportal: set DP_URL, DP_USER and DP_PASS (values are never printed)', file=sys.stderr)
        return 2
    dp = DevicePortal(url, user, pw, os.environ.get('DP_TLS_SHA256'), a.timeout)
    ident, apps = manifest_identity(a.manifest)
    try:
        if a.cmd == 'info':
            info = dp.prime()
            fam = dp.json('GET', '/api/os/devicefamily')
            print(json.dumps({'OsVersion': info.get('OsVersion'), 'OsEdition': info.get('OsEdition'),
                              'Platform': info.get('Platform'), 'DeviceType': fam.get('DeviceType')}, indent=1))
            if dp.fingerprint:
                print(f'TLS sha256 fingerprint: {dp.fingerprint}  (export DP_TLS_SHA256=... to pin)')
        elif a.cmd == 'install':
            r = dp.install(a.appx, a.extra, wait=a.wait)
            print(json.dumps(r))
            p = dp.find(ident)
            print(f'installed: {p.get("PackageFullName")}')
        elif a.cmd == 'list':
            pk = sorted(dp.packages(), key=lambda p: not p.get('PackageFullName', '').startswith(ident + '_'))
            for p in pk:
                v = p.get('Version', {})
                print(f'{p.get("PackageFullName")}\t{p.get("Name")}\t'
                      f'{v.get("Major")}.{v.get("Minor")}.{v.get("Build")}.{v.get("Revision")}\tappid={p.get("PackageRelativeId")}')
        elif a.cmd == 'launch':
            full = dp.find(ident)['PackageFullName']
            appid = a.app or apps[0]
            if appid not in apps:
                raise DPError(f'app {appid!r} not in manifest applications {apps}')
            dp.launch(full, appid)
            print(f'launched {full} app {appid}')
        elif a.cmd == 'stop':
            full = dp.find(ident)['PackageFullName']
            dp.stop(full)
            print(f'stopped {full}')
        elif a.cmd == 'files':
            full = dp.find(ident)['PackageFullName']
            for it in dp.files(full, a.path):
                kind = 'dir ' if it.get('SubPath') or it.get('Type') == 16 else 'file'
                print(f'{kind}\t{it.get("FileSize", 0)}\t{it.get("Name")}')
        elif a.cmd in ('pull', 'pull-logs'):
            full = dp.find(ident)['PackageFullName']
            if a.cmd == 'pull':
                names = list(a.names)
            else:
                names = [it['Name'] for it in dp.files(full, a.path)
                         if not it.get('SubPath') and re.search(r'\.(log|txt|json)$', it.get('Name', ''), re.I)]
                if not names:
                    raise DPError(f'no log files in {a.path} (app not run yet, or log creation failed)')
            outdir = Path(a.out)
            for n in names:
                if '/' in n or '\\' in n or n in ('.', '..'):
                    raise DPError(f'bad file name {n!r}')
                size = dp.pull(full, a.path, n, outdir / n)
                print(f'pulled {a.path}\\{n} -> {outdir / n} ({size} bytes)')
        elif a.cmd == 'settings':
            data = dp.json('GET', '/ext/settings')
            items = data.get('Settings', data if isinstance(data, list) else [])
            for s in items:
                mark = '  <- game/app-type candidate' if re.search(r'(?i)game|apptype|applicationtype', s.get('Name', '')) else ''
                print(f'{s.get("Category")}\t{s.get("Name")}\t= {s.get("Value")!r}\toptions={s.get("Options")}'
                      f'\treboot={s.get("RequiresReboot")}{mark}')
        elif a.cmd == 'set-setting':
            dp.prime()
            dp.request('PUT', f'/ext/settings/{a.name}', body=json.dumps({'Value': a.value}).encode(),
                       headers={'Content-Type': 'application/json'})
            print(f'set {a.name} = {a.value!r}')
        elif a.cmd == 'screenshot':
            _, data = dp.request('GET', '/ext/screenshot', {'download': 'true'})
            Path(a.out).write_bytes(data)
            print(f'screenshot -> {a.out} ({len(data)} bytes)')
        elif a.cmd == 'uninstall':
            if a.confirm != 'yes':
                raise DPError('uninstall removes the app and its LocalState from the console; pull logs first, then: uninstall yes')
            full = dp.find(ident)['PackageFullName']
            dp.uninstall(full)
            print(f'uninstalled {full}')
        elif a.cmd == 'reboot':
            if a.confirm != 'now':
                raise DPError('reboot restarts the console (a running game is closed); confirm with: reboot now')
            dp.prime()
            dp.request('POST', '/api/control/restart')
            print('console restart requested')
    except DPError as e:
        print('deploy-devportal: ' + dp.redact(str(e)), file=sys.stderr)
        return 1
    return 0

if __name__ == '__main__':
    sys.exit(main())
