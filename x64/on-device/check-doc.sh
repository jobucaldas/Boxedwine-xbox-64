#!/usr/bin/env bash
# Install docs vs built package: every rung, file and step the docs name must
# exist in the package, and every package entry/rung must be documented.
# Build-only check on Linux; it runs nothing on a console and no Wine.
#   usage: check-doc.sh [KIT_DIR]   (default out/kit; built with `make kit` if absent)
#   TOMORROW=path overrides /workspace/x64-run/ON-DEVICE-TOMORROW.md
set -euo pipefail
cd -- "$(dirname -- "$0")"
kit=${1:-out/kit}
tomorrow=${TOMORROW:-/workspace/x64-run/ON-DEVICE-TOMORROW.md}
if [[ -z "${1:-}" && ! -d out/kit ]]; then
    make --no-print-directory kit >/dev/null
fi
test -d "$kit" && test -f README.md && test -f "$tomorrow"
exec python3 -I - "$PWD" "$(cd -- "$kit" && pwd)" "$tomorrow" <<'PY'
import os, re, shutil, subprocess, sys, tempfile
import xml.etree.ElementTree as ET
from pathlib import Path

ondev, kit, tomorrow = Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3])
repo = ondev.parent.parent
out = kit.parent
docs = {'README.md': (ondev / 'README.md').read_text(), 'ON-DEVICE-TOMORROW.md': tomorrow.read_text()}
mismatches, checks = [], 0

def check(ok, doc, what):
    global checks
    checks += 1
    if not ok:
        mismatches.append(f'MISMATCH {doc}: {what}')

def mentioned(text, word):
    return re.search(r'(?<![\w.-])' + re.escape(word) + r'(?![\w-])', text) is not None

# ---- package facts -------------------------------------------------------
# Rungs: run a copy of the packaged ladder (not --offline, empty LocalState,
# no adapters) so every stage is SKIP and nothing executes.
tmp = Path(tempfile.mkdtemp(prefix='check-doc.'))
try:
    for layout in ('kit', 'x64/on-device/out'):
        (tmp / layout).mkdir(parents=True)
        shutil.copy2(kit / 'ladder', tmp / layout / 'ladder')
        run = subprocess.run([str(tmp / layout / 'ladder')], capture_output=True, text=True, timeout=30)
        check(run.returncode == 0, 'package', f'{layout}/ladder exit {run.returncode}')
    stages = [l.split()[1:3] for l in run.stdout.splitlines() if l.startswith('STAGE ')]
    # Default log locations the docs promise for hosted runs.
    check((tmp / 'kit/LocalState/ladder.log').is_file(), 'package', 'kit ladder does not write <kit>/LocalState/ladder.log')
    check((tmp / 'x64/on-device/LocalState/ladder.log').is_file(), 'package', 'source ladder does not write x64/on-device/LocalState/ladder.log')
    alt = tmp / 'alt'
    subprocess.run([str(tmp / 'kit/ladder'), '--local-state', str(alt)], capture_output=True, timeout=30)
    check((alt / 'ladder.log').is_file(), 'package', '--local-state DIR does not write DIR/ladder.log')
    ladder_help = subprocess.run([str(kit / 'ladder'), '--help'], capture_output=True, text=True).stdout
finally:
    shutil.rmtree(tmp)
rung_ids = [s[0] for s in stages]
rungs_bin = (kit / 'payload/rungs.exe').read_bytes()
rungs_strings = rungs_bin.decode('latin-1') + rungs_bin.decode('utf-16-le', 'ignore')
manifest = ET.parse(kit / 'payload/AppxManifest.xml').getroot()
ns = {'f': 'http://schemas.microsoft.com/appx/manifest/foundation/windows10',
      'uap': 'http://schemas.microsoft.com/appx/manifest/uap/windows10'}
identity = manifest.find('f:Identity', ns).attrib
family = manifest.find('f:Dependencies/f:TargetDeviceFamily', ns).attrib
capabilities = [c.attrib['Name'] for c in manifest.iter() if c.tag.endswith('}Capability')]
applications = [(a.attrib['Id'], a.attrib['EntryPoint'], a.attrib['Executable'],
                 a.find('uap:VisualElements', ns).attrib['DisplayName'])
                for a in manifest.find('f:Applications', ns)]
make_targets = set()
for mk in (ondev / 'Makefile', repo / 'x64/uwp-host/harness/Makefile'):
    for line in mk.read_text().splitlines():
        m = re.match(r'^([\w./$()-][^:=\s]*(?:\s+[^:=\s]+)*)\s*:(?!=)', line)
        if m:
            make_targets.update(t.replace('$(OUT)', 'out') for t in m.group(1).split())
        if line.startswith('.PHONY:'):
            make_targets.update(line.split()[1:])
build_text = ''.join(p.read_text() for p in [ondev / 'Makefile', *ondev.glob('*.sh')])

def in_kit(rel):
    """Doc path (repo-, on-device- or out-relative) -> exists in package?"""
    rel = rel.rstrip('/')
    rel = re.sub(r'^(\./)?(x64/on-device/)?(out/)?', '', rel)
    if '*' in rel:
        return any(kit.glob(rel)) or any((kit / 'payload').glob(rel))
    if '/' not in rel and rel.endswith(('.exe', '.dll', '.so', '.py', '.tsv')):
        return any(kit.rglob(rel))
    return (kit / rel).exists() or (kit / 'payload' / rel).exists()

# ---- docs -> package -------------------------------------------------------
RUNTIME = ('LocalState', 'BoxedwineDev.appx', 'ladder-App.log', 'ladder-Game.log',
           'ladder-native.log', 'ladder.log', 'big-file.bin', 'smoke-input.txt',
           'adapters/', 'private/', 'payload/Megabonk.exe', 'Megabonk.exe')
for doc, text in docs.items():
    for tok in re.findall(r'`([^`\n]+)`', text):
        if ' ' in tok or '<' in tok or '$' in tok or tok.startswith(('http', '/path/to', '/absolute/path')):
            continue
        if tok.startswith(RUNTIME) or tok.endswith(('.log',)):
            continue  # console/runtime outputs; checked as send-back items below
        if tok.startswith('/'):
            p = Path(tok.rstrip('/'))
            check(p.exists(), doc, f'path `{tok}` does not exist')
            m = re.match(r'^(/workspace/wt/[^/]+/x64/on-device)/(out|README\.md|LocalState)', tok)
            if m:
                check(Path(m.group(1)).resolve() == ondev.resolve(), doc,
                      f'`{tok}` names another worktree than the checked package {ondev}')
                if m.group(2) == 'out':
                    sub = tok[len(m.group(1)) + 1:]
                    check(in_kit(sub) or (ondev / sub).exists(), doc, f'`{tok}` not in package')
            continue
        if re.match(r'^(\./)?(x64/on-device/)?out/', tok) or tok.startswith(('payload', 'Assets/', 'test-cert', 'combo/')) \
                or tok in ('ladder', 'README.md', 'KIT-NOTES.txt', 'kit-manifest.txt', 'SHA256SUMS', 'probes/') \
                or re.fullmatch(r'[\w.-]+\.(exe|dll|so)', tok):
            name = re.sub(r'^(\./)?(x64/on-device/)?out/', '', tok).rstrip('/')
            ok = in_kit(tok) or (out / name).exists() or ('out/' + name) in build_text \
                 or name in make_targets or ('$(OUT)/' + name) in build_text
            check(ok, doc, f'`{tok}` is neither in the package nor produced by the build')
        elif re.match(r'^(\./)?x64/', tok):
            check((repo / tok.lstrip('./')).exists(), doc, f'repo path `{tok}` does not exist')
        elif tok.endswith('.sh'):
            check((ondev / tok).exists() or any(ondev.rglob(tok)), doc, f'script `{tok}` does not exist')

    # Rungs named in the doc must be real stages (ranges like a–h, n0–n8 expanded).
    named = set()
    for a, x, b, y in re.findall(r'\b([a-hnp])(\d?)[–-]([a-hnp])(\d?)\b', text):
        if x and y and a == b:
            named.update(f'{a}{i}' for i in range(int(x), int(y) + 1))
        elif not x and not y:
            named.update(chr(c) for c in range(ord(a), ord(b) + 1))
    named.update(re.findall(r'(?<![\w/.-])([np]\d)(?![\w-])', text))
    named.update(re.findall(r'\b[Ss]tages? ([a-h])\b', text))
    for r in sorted(named):
        check(r in rung_ids, doc, f'rung {r} is not a packaged ladder stage')
    # Stage-count claims.
    words = {'sixteen': 16, 'twenty-four': 24, 'twenty-three': 23, 'twenty-five': 25}
    for n in re.findall(r'\b(\d+|[a-z]+(?:-[a-z]+)?) STAGE lines', text):
        count = int(n) if n.isdigit() else words.get(n, -1)
        check(count == len(stages), doc, f'claims {n} STAGE lines; packaged ladder prints {len(stages)}')
    # Flags must exist on the ladder or rungs.exe.
    for flag in sorted(set(re.findall(r'(?<![\w-])--[a-z][a-z-]+', text))):
        check(flag in ladder_help or flag in rungs_strings, doc, f'flag {flag} not accepted by ladder or rungs.exe')
    # make targets used in steps.
    for target_list in re.findall(r'make -C x64/(?:on-device|uwp-host/harness) ([\w -]+?)(?:`|&&|$)', text, re.M):
        for t in target_list.split():
            check(t in make_targets, doc, f'make target {t} does not exist')
    # Identity facts.
    for cn in set(re.findall(r'CN=\w+', text)):
        check(cn == identity['Publisher'], doc, f'{cn} differs from manifest Publisher {identity["Publisher"]}')
    for v in set(re.findall(r'MinVersion (\d+\.\d+\.\d+\.\d+)', text)):
        check(v == family['MinVersion'], doc, f'MinVersion {v} differs from manifest {family["MinVersion"]}')
    if 'Windows.Xbox' in text:
        check(family['Name'] == 'Windows.Xbox', doc, 'manifest target family is not Windows.Xbox')
    for ep in set(re.findall(r'Boxedwine\.\w+', text)):
        check(ep in [a[1] for a in applications], doc, f'EntryPoint {ep} not in manifest')
    if 'no dependency packages' in text:
        check(manifest.find('f:Dependencies/f:PackageDependency', ns) is None, doc, 'manifest has PackageDependency')

# ---- package -> docs -------------------------------------------------------
readme, tom = docs['README.md'], docs['ON-DEVICE-TOMORROW.md']
for doc, text in docs.items():
    for r in rung_ids:
        hit = r == 'combo' and 'combo' in text
        for a, x, b, y in re.findall(r'\b([a-hnp])(\d?)[–-]([a-hnp])(\d?)\b', text):
            if x and y and a == b == r[0] and r[1:].isdigit() and int(x) <= int(r[1:]) <= int(y):
                hit = True
            if not x and not y and len(r) == 1 and a <= r <= b:
                hit = True
        hit = hit or mentioned(text, r) and r not in tuple('abcdefgh')
        check(hit, doc, f'packaged rung {r} is not documented')
    for name in ('payload', 'test-cert.cer', 'test-cert.pfx', 'kit.zip', 'SHA256SUMS'):
        check(name in text, doc, f'package entry {name} is not documented')
    for _, _, _, display in applications:
        check(display in text or doc == 'README.md' and display == 'Boxedwine Dev Mode' and 'Dev Home' in text,
              doc, f'app tile "{display}" is not documented')
    # What João must send back: both logs, their exact paths.
    m = re.search(r'^## (?:How to send results back|What to send back).*?(?=^## |\Z)', text, re.M | re.S)
    check(m is not None, doc, 'no send-back section')
    back = m.group(0) if m else ''
    for need in ('LocalState/ladder-native.log', 'ladder-App.log', 'ladder-Game.log',
                 'x64/on-device/LocalState/ladder.log', '<kit>/LocalState/ladder.log', 'DIR/ladder.log'):
        check(need in back, doc, f'send-back section does not name `{need}`')
for top in sorted(p.name for p in kit.iterdir()):
    check(mentioned(readme, top), 'README.md', f'kit entry {top} is not documented')
for sub in ('payload', 'payload/combo'):
    for p in sorted((kit / sub).iterdir()):
        check(mentioned(readme, p.name), 'README.md', f'{sub}/{p.name} is not documented')
for cap in capabilities:
    check(f'`{cap}`' in readme, 'README.md', f'capability {cap} is not documented')
for app_id, ep, exe, _ in applications:
    check(ep in readme and exe in readme, 'README.md', f'application {app_id} ({ep}, {exe}) is not documented')
for f in ('ladder-native.log', 'big-file.bin', 'smoke-input.txt', 'Boxedwine.BigFile'):
    check(f in rungs_strings, 'package', f'rungs.exe does not reference {f}')

# ---- steps ---------------------------------------------------------------
makeappx = 'MakeAppx pack /d payload /p BoxedwineDev.appx'
signtool = 'SignTool sign /fd SHA256 /f test-cert.pfx BoxedwineDev.appx'
notes = ' '.join((kit / 'payload/PACKAGING-NOTES.txt').read_text().split())
for doc, text in docs.items():
    flat = ' '.join(text.split())
    check(makeappx in flat and signtool in flat, doc, 'MakeAppx/SignTool step differs from the package notes')
check(makeappx in notes and signtool in notes, 'package', 'PACKAGING-NOTES.txt MakeAppx/SignTool step differs')
check((kit / 'payload/AppxManifest.xml').is_file(), 'package', 'MakeAppx /d payload has no AppxManifest.xml')
for bad in ('*.pfx', '*.token', 'LocalState', 'private'):
    check(not any((kit / 'payload').rglob(bad)), 'package', f'payload contains {bad}')
subject = subprocess.run(['openssl', 'x509', '-inform', 'DER', '-in', str(kit / 'test-cert.cer'), '-noout',
                          '-subject', '-nameopt', 'RFC2253'], capture_output=True, text=True).stdout.strip()
check(subject == 'subject=' + identity['Publisher'], 'package', f'test-cert.cer {subject} != manifest Publisher')
check((kit / 'README.md').read_text() == readme, 'package', 'kit README.md is stale: rebuild with `make kit`')
check((kit / 'kit-manifest.txt').read_text() == (ondev / 'kit-manifest.txt').read_text(),
      'package', 'kit-manifest.txt differs from the kit copy: rebuild with `make kit`')
listed = {l.split(' | ')[0] for l in (kit / 'kit-manifest.txt').read_text().splitlines() if '| MISSING' not in l}
files = {p.relative_to(kit).as_posix() for p in kit.rglob('*') if p.is_file()}
check(listed == files, 'package', f'kit-manifest.txt vs kit files: {sorted(listed ^ files)[:5]}')
if (out / 'SHA256SUMS').is_file():
    rc = subprocess.run(['sha256sum', '--quiet', '-c', 'SHA256SUMS'], cwd=out, capture_output=True).returncode
    check(rc == 0, 'package', 'out/SHA256SUMS does not verify')
else:
    check(False, 'package', 'out/SHA256SUMS missing')

for m in mismatches:
    print(m)
print(f'check-doc: {len(mismatches)} mismatches ({checks} checks; {len(stages)} rungs; kit {kit})')
sys.exit(1 if mismatches else 0)
PY
