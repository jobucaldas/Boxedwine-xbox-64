#!/usr/bin/env python3
"""Extract the bounded THROUGH-uwph run; no stock Wine client evidence."""
from pathlib import Path
from collections import Counter
import re, shutil
E = Path('/workspace/x64-run/lanes/B22/o2')
repo = Path(__file__).resolve().parents[5]
source = Path('/workspace/wt/B20/x64/uwp-host/harness/out/b20/pfx-wow64-uwph/drive_c/Program Files (x86)/Steam/logs/bootstrap_log.txt')
shutil.copyfile(source, E / 'bootstrap_log.txt')
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
    execs = [l for l in (E / (phase + '.strace')).read_text(errors='replace').splitlines() if 'execve(' in l and '= 0' in l]
    for line in execs: add(phase, 'linux-exec', 'successful execve', 1, line)
# Only the latest bootstrap launch, rather than earlier B20 observations.
boot = (E / 'bootstrap_log.txt').read_text(errors='replace')
boot = boot[boot.rfind('Startup - updater built'):]
for line in boot.splitlines():
    if line.strip(): add('client', 'steam-bootstrap', 'bootstrap_log.txt', 1, line)
add('client', 'not-reached', 'client UI / steamclient.dll', 0, 'Bootstrap manifest download blocked; no client UI observed')
out = repo / 'x64/uwp-host/harness/surface-trace-steam-client.tsv'
out.write_text('\n'.join('\t'.join(row) for row in rows) + '\n')
print(out)
