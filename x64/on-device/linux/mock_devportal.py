#!/usr/bin/env python3
"""Mock Xbox Device Portal for the B60 offline deploy test (HTTPS, self-signed).

Enforces what the real portal enforces that our client depends on: Basic auth,
CSRF-Token cookie + X-CSRF-Token header on writes, multipart package upload,
204-then-200 install state, base64 (\"hex64\") appid/package on taskmanager.
Writes a JSON event log (no credentials) to --events for the test to assert on.
"""
import argparse, base64, hashlib, json, ssl, sys, threading
from email.parser import BytesParser
from email.policy import HTTP
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlsplit

FULL = 'Joao.Boxedwine.DevMode_0.1.0.0_x64__mockpublisherid'
CSRF = 'mock-csrf-' + 'a1b2c3'

class State:
    lock = threading.Lock()
    installed = False
    polls = 0
    events = []
    settings = [{'Name': 'DefaultApplicationType', 'Value': 'App', 'Type': 'Select', 'Category': 'Preferences',
                 'Options': ['App', 'Game'], 'RequiresReboot': 'Yes', 'Disabled': 'No'},
                {'Name': 'HostName', 'Value': 'XBOX', 'Type': 'Text', 'Category': 'General',
                 'RequiresReboot': 'Yes', 'Disabled': 'No'}]
    files = {'ladder-native.log': b'STAGE n0 mock SKIP\nSTAGE n10 mock PASS\n', 'ladder-App.log': b'mock app log\n'}

class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass
    def ev(self, **kw):
        with State.lock:
            State.events.append(kw)
            open(self.server.events_path, 'w').write(json.dumps(State.events, indent=1))
    def send(self, code, body=b'', ctype='application/json'):
        self.send_response(code)
        self.send_header('Set-Cookie', f'CSRF-Token={CSRF}; Path=/; Secure')
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)
    def authed(self):
        want = 'Basic ' + base64.b64encode(self.server.creds.encode()).decode()
        if self.headers.get('Authorization') != want:
            self.ev(path=self.path.split('?')[0], status=401); self.send(401, b'{}'); return False
        if self.command != 'GET':
            cookie = self.headers.get('Cookie', '')
            if f'CSRF-Token={CSRF}' not in cookie or self.headers.get('X-CSRF-Token') != CSRF:
                self.ev(path=self.path.split('?')[0], status=403, why='csrf'); self.send(403, b'{"Reason":"CSRF"}'); return False
        return True
    def body(self):
        n = int(self.headers.get('Content-Length', 0))
        return self.rfile.read(n)
    def route(self):
        if not self.authed():
            return
        u = urlsplit(self.path); q = {k: v[0] for k, v in parse_qs(u.query).items()}; p = u.path
        m = self.command
        if m == 'GET' and p == '/api/os/info':
            return self.send(200, json.dumps({'ComputerName': 'XBOX', 'OsEdition': 'Xbox', 'OsVersion': '10.0.26100.mock', 'Platform': 'Xbox Series S'}).encode())
        if m == 'GET' and p == '/api/os/devicefamily':
            return self.send(200, b'{"DeviceType":"Windows.Xbox"}')
        if m == 'POST' and p == '/api/app/packagemanager/package':
            raw = self.body()
            msg = BytesParser(policy=HTTP).parsebytes(b'Content-Type: ' + self.headers['Content-Type'].encode() + b'\r\n\r\n' + raw)
            parts = [(pt.get_filename(), pt.get_payload(decode=True)) for pt in msg.iter_parts()]
            self.ev(path=p, package=q.get('package'), parts=[(n, len(d), hashlib.sha256(d).hexdigest()) for n, d in parts])
            ok = parts and parts[0][0] == q.get('package') and parts[0][1][:2] == b'PK'
            with State.lock:
                State.installed = bool(ok); State.polls = 0
            return self.send(200 if ok else 400, b'')
        if m == 'GET' and p == '/api/app/packagemanager/state':
            with State.lock:
                State.polls += 1
                polls = State.polls
            if polls < 3:
                return self.send(204)
            return self.send(200, json.dumps({'Code': 0, 'CodeText': 'Success', 'Reason': '', 'Success': True}).encode())
        if m == 'GET' and p == '/api/app/packagemanager/packages':
            pk = [{'Name': 'Microsoft.Other', 'PackageFullName': 'Microsoft.Other_1.0.0.0_x64__8wekyb3d8bbwe', 'PackageRelativeId': 'Microsoft.Other_8wekyb3d8bbwe!App', 'Version': {'Major': 1, 'Minor': 0, 'Build': 0, 'Revision': 0}}]
            if State.installed:
                pk.append({'Name': 'Boxedwine Dev Mode', 'PackageFullName': FULL, 'PackageRelativeId': 'Joao.Boxedwine.DevMode_mockpublisherid!App', 'Version': {'Major': 0, 'Minor': 1, 'Build': 0, 'Revision': 0}})
            return self.send(200, json.dumps({'InstalledPackages': pk}).encode())
        if p == '/api/taskmanager/app' and m in ('POST', 'DELETE'):
            pkg = base64.b64decode(q.get('package', '')).decode()
            appid = base64.b64decode(q['appid']).decode() if 'appid' in q else None
            self.ev(path=p, method=m, package=pkg, appid=appid, forcestop=q.get('forcestop'))
            return self.send(200 if pkg == FULL and (m == 'DELETE' or appid in ('App', 'BigFile')) else 400, b'')
        if m == 'GET' and p == '/api/filesystem/apps/files':
            if q.get('packagefullname') != FULL or q.get('knownfolderid') != 'LocalAppData':
                return self.send(404, b'{}')
            items = [{'Name': n, 'FileSize': len(d), 'Type': 32, 'CurrentDir': q.get('path')} for n, d in State.files.items()]
            items.append({'Name': 'private', 'SubPath': '\\LocalState\\private', 'Type': 16})
            return self.send(200, json.dumps({'Items': items}).encode())
        if m == 'GET' and p == '/api/filesystem/apps/file':
            self.ev(path=p, file=q.get('filename'), dir=q.get('path'))
            d = State.files.get(q.get('filename')) if q.get('packagefullname') == FULL else None
            return self.send(200, d, 'application/octet-stream') if d is not None else self.send(404, b'{}')
        if m == 'GET' and p == '/api/debug/dump/usermode/dumps':
            self.ev(path=p, method=m, status=200)
            return self.send(200, json.dumps({'Dumps': [{'PackageFullName': FULL, 'FileName': 'mock.dmp'}]}).encode())
        if m == 'GET' and p == '/api/debug/dump/usermode/crashdump':
            self.ev(path=p, method=m, query=q, status=404)
            return self.send(404, b'{}')
        if m == 'GET' and p == '/ext/settings':
            return self.send(200, json.dumps({'Settings': State.settings}).encode())
        if m == 'PUT' and p.startswith('/ext/settings/'):
            name = p.rsplit('/', 1)[1]; val = json.loads(self.body())['Value']
            for s in State.settings:
                if s['Name'] == name:
                    s['Value'] = val; self.ev(path=p, value=val); return self.send(200, b'')
            return self.send(404, b'{}')
        if m == 'POST' and p == '/api/control/restart':
            self.ev(path=p, method=m); return self.send(200, b'')
        if m == 'GET' and p == '/ext/screenshot':
            return self.send(200, b'\x89PNG\r\n\x1a\nmock', 'image/png')
        if m == 'DELETE' and p == '/api/app/packagemanager/package':
            self.ev(path=p, method=m, package=q.get('package'))
            State.installed = False
            return self.send(200, b'')
        self.send(404, b'{}')
    do_GET = do_POST = do_PUT = do_DELETE = route

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--cert'); ap.add_argument('--key'); ap.add_argument('--events', required=True)
    ap.add_argument('--port-file', required=True)
    a = ap.parse_args()
    srv = ThreadingHTTPServer(('127.0.0.1', 0), H)
    srv.creds = sys.stdin.readline().rstrip('\n')   # "user:pass" via stdin, not argv
    srv.events_path = a.events
    srv.handle_error = lambda *a: None   # client may drop the TLS session (pin test)
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER); ctx.load_cert_chain(a.cert, a.key)
    srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    open(a.port_file, 'w').write(str(srv.server_address[1]))
    srv.serve_forever()

if __name__ == '__main__':
    main()
