#!/usr/bin/env python3
"""Extract the bounded THROUGH-uwph run; no stock Wine client evidence."""
from pathlib import Path
from collections import Counter
import re, sys
E = Path(sys.argv[1]).resolve()
repo = Path(__file__).resolve().parents[5]
source = Path(sys.argv[2])
before = (E / 'bootstrap.before').read_bytes()
current = source.read_bytes() if source.exists() else b''
if not current.startswith(before):
    raise RuntimeError('Bootstrap log rotated/truncated; cannot attribute log to this run')
# Only bytes appended during this launch can be used as bootstrap evidence.
(E / 'bootstrap_log.txt').write_bytes(current[len(before):])

def successful_execs(lines):
    pending = {}
    for line in lines:
        match = re.match(r'(\d+)\s+(.*)', line)
        if not match:
            continue
        pid, call = match.groups()
        if 'execve(' in call and '<unfinished ...>' in call:
            pending[pid] = call.split('<unfinished ...>')[0]
            continue
        if call.startswith('<... execve resumed>'):
            start = pending.pop(pid, None)
            if start is None:
                continue
            call = start + call[len('<... execve resumed>'):]
        if call.startswith('execve(') and re.search(r'= 0\s*$', call):
            yield pid + ' ' + call
rows = [('phase', 'kind', 'symbol_or_surface', 'count', 'evidence')]
def add(phase, kind, surface, count, evidence):
    rows.append(tuple(str(x).replace('\t', ' ').replace('\r', '').replace('\n', ' ') for x in (phase,kind,surface,count,evidence)))
for phase in ('probe', 'client'):
    lines = (E / (phase + '.log')).read_text(errors='replace').splitlines()
    add(phase, 'run', 'net_probe-i386.exe' if phase == 'probe' else 'Steam.exe -no-browser -silent', 1, 'THROUGH uwph; exit=' + (E / (phase + '.exit')).read_text().strip())
    for line, count in Counter(l for l in lines if any(x in l for x in ('[launcher]', 'SUMMARY net_probe', 'PASS ', 'err:', 'failed to load', 'experimental wow64 mode'))).items():
        add(phase, 'diagnostic', 'loader/probe/Wine', count, line)
    for dll in ('WS2_32.dll', 'steamclient.dll'):
        matches = [l for l in lines if dll.lower() in l.lower() and ('Loaded ' in l or 'PROCESS_ATTACH' in l)]
        add(phase, 'module', dll, len(matches), matches[-1] if matches else 'No load/attach observed')
    totals = Counter()
    reports = list(E.glob(phase + '.counters.*'))
    for report in reports:
        for line in report.read_text().splitlines():
            fields = line.split()
            if fields and fields[0] == 'total': totals[fields[1]] += int(fields[2])
    add(phase, 'uwph-routed', 'shim totals', sum(totals.values()), str(dict(totals)) + '; reports=' + str(len(reports)))
    execs = successful_execs((E / (phase + '.strace')).read_text(errors='replace').splitlines())
    for line in execs: add(phase, 'linux-exec', 'successful execve', 1, line)
# Only the latest bootstrap launch, rather than earlier B20 observations.
boot = (E / 'bootstrap_log.txt').read_text(errors='replace')
# This file already contains only this run's appended bytes.
if not boot.strip():
    add('client', 'steam-bootstrap', 'bootstrap_log.txt', 0, 'No bootstrap log bytes appended during this run')
for line in boot.splitlines():
    if line.strip(): add('client', 'steam-bootstrap', 'bootstrap_log.txt', 1, line)
add('client', 'not-reached', 'client UI / steamclient.dll', 0, 'Bootstrap manifest download blocked; no client UI observed')
out = repo / 'x64/uwp-host/harness/surface-trace-steam-client.tsv'
out.write_text('\n'.join('\t'.join(row) for row in rows) + '\n')
print(out)
