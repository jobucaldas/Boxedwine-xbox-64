#!/usr/bin/env python3
"""Offline structural check of a signed .appx built on Linux (B60).

Checks, without any console or Windows tool:
  * the package is a readable zip with AppxManifest.xml, AppxBlockMap.xml,
    [Content_Types].xml and (unless --unsigned) AppxSignature.p7x;
  * every AppxBlockMap.xml <File> exists and each 64 KiB block's SHA-256
    matches the uncompressed bytes; every payload member is in the block map;
  * [Content_Types].xml covers every member (Default by extension or Override);
  * Identity Publisher equals the signing certificate subject (--cert);
  * optional --payload DIR: package files are byte-identical to DIR's files;
  * no private material (*.pfx, *.pem, *.key, tokens, LocalState/, combo/);
  * osslsigncode verify -CAfile <cert> succeeds (unless --unsigned).
Exit 0 and print 'APPX CHECK PASS' only if all checks pass.
"""
import argparse, base64, hashlib, re, subprocess, sys, tempfile, zipfile
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath

BLOCK = 65536
FOOT = {'AppxBlockMap.xml', '[Content_Types].xml', 'AppxSignature.p7x', 'AppxMetadata/CodeIntegrity.cat'}
BM = '{http://schemas.microsoft.com/appx/2010/blockmap}'
CT = '{http://schemas.openxmlformats.org/package/2006/content-types}'
MF = '{http://schemas.microsoft.com/appx/manifest/foundation/windows10}'
FORBIDDEN = re.compile(r'(?i)(\.pfx$|\.pem$|\.key$|token|(^|/)LocalState(/|$)|(^|/)combo(/|$)|(^|/)private(/|$))')

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('appx')
    ap.add_argument('--cert', help='DER test-cert.cer used to sign (publisher + signature check)')
    ap.add_argument('--payload', help='staged directory the package was built from')
    ap.add_argument('--unsigned', action='store_true')
    a = ap.parse_args()
    errors, checks = [], 0
    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            errors.append(what)

    try:
        z = zipfile.ZipFile(a.appx)
    except Exception as e:  # noqa: BLE001
        print(f'APPX CHECK FAIL: not a zip: {e}'); return 1
    names = [i.filename for i in z.infolist()]
    check(len(names) == len(set(names)), 'duplicate zip members')
    for need in ('AppxManifest.xml', 'AppxBlockMap.xml', '[Content_Types].xml') + (() if a.unsigned else ('AppxSignature.p7x',)):
        check(need in names, f'missing {need}')
    if errors:
        print('\n'.join('FAIL ' + e for e in errors)); print('APPX CHECK FAIL'); return 1
    check(z.testzip() is None, 'zip CRC error')

    # ---- block map ----
    bm = ET.fromstring(z.read('AppxBlockMap.xml'))
    check(bm.get('HashMethod') == 'http://www.w3.org/2001/04/xmlenc#sha256', f'HashMethod {bm.get("HashMethod")}')
    mapped = {}
    for f in bm.findall(BM + 'File'):
        name = f.get('Name').replace('\\', '/')
        mapped[name] = f
        if name not in names:
            errors.append(f'blockmap file {name} not in zip'); continue
        data = z.read(name)
        check(int(f.get('Size')) == len(data), f'{name}: Size {f.get("Size")} != {len(data)}')
        blocks = f.findall(BM + 'Block')
        check(len(blocks) == (len(data) + BLOCK - 1) // BLOCK, f'{name}: {len(blocks)} blocks for {len(data)} bytes')
        for i, b in enumerate(blocks):
            digest = base64.b64encode(hashlib.sha256(data[i * BLOCK:(i + 1) * BLOCK]).digest()).decode()
            check(b.get('Hash') == digest, f'{name}: block {i} hash mismatch')
    payload_members = [n for n in names if n not in FOOT]
    for n in payload_members:
        check(n in mapped, f'zip member {n} missing from AppxBlockMap.xml')

    # ---- content types ----
    ct = ET.fromstring(z.read('[Content_Types].xml'))
    defaults = {d.get('Extension').lower() for d in ct.findall(CT + 'Default')}
    overrides = {o.get('PartName') for o in ct.findall(CT + 'Override')}
    for n in names:
        if n == '[Content_Types].xml':
            continue
        ext = PurePosixPath(n).suffix.lstrip('.').lower()
        check('/' + n in overrides or ext in defaults, f'no content type for {n}')
    check('/AppxBlockMap.xml' in overrides, 'no Override for /AppxBlockMap.xml')
    if not a.unsigned:
        check('/AppxSignature.p7x' in overrides, 'no Override for /AppxSignature.p7x')

    # ---- forbidden content ----
    for n in names:
        check(not FORBIDDEN.search(n), f'forbidden member {n}')

    # ---- manifest identity ----
    ident = ET.fromstring(z.read('AppxManifest.xml')).find(MF + 'Identity')
    check(ident is not None and ident.get('ProcessorArchitecture') == 'x64', 'Identity is not x64')
    publisher = ident.get('Publisher') if ident is not None else None
    if a.cert:
        subj = subprocess.run(['openssl', 'x509', '-inform', 'DER', '-in', a.cert, '-noout', '-subject',
                               '-nameopt', 'RFC2253'], capture_output=True, text=True).stdout.strip()
        check(subj == f'subject={publisher}', f'cert {subj!r} != manifest Publisher {publisher!r}')

    # ---- payload equality ----
    if a.payload:
        root = Path(a.payload)
        files = {p.relative_to(root).as_posix() for p in root.rglob('*') if p.is_file()}
        check(set(payload_members) == files, f'package vs payload: {sorted(set(payload_members) ^ files)[:6]}')
        for n in sorted(files & set(payload_members)):
            check(z.read(n) == (root / n).read_bytes(), f'{n} differs from payload')

    # ---- signature ----
    if not a.unsigned:
        if not a.cert:
            errors.append('--cert is required to verify the signature')
        else:
            with tempfile.NamedTemporaryFile(suffix='.pem') as pem:
                subprocess.run(['openssl', 'x509', '-inform', 'DER', '-in', a.cert, '-out', pem.name], check=True)
                v = subprocess.run(['osslsigncode', 'verify', '-CAfile', pem.name, '-in', a.appx],
                                   capture_output=True, text=True)
            out = v.stdout + v.stderr
            check(v.returncode == 0 and 'Signature verification: ok' in out,
                  'osslsigncode verify failed: ' + ' | '.join(l for l in out.splitlines() if 'fail' in l.lower() or 'error' in l.lower())[:300])
            want = (publisher or '').replace('CN=', '/CN=')
            check(f'Subject: {want}' in out, f'signer subject is not {want}')

    for e in errors:
        print('FAIL', e)
    print(f'appx_check: {len(errors)} failures ({checks} checks; {len(payload_members)} payload files; {Path(a.appx).name})')
    print('APPX CHECK PASS' if not errors else 'APPX CHECK FAIL')
    return 1 if errors else 0

if __name__ == '__main__':
    sys.exit(main())
