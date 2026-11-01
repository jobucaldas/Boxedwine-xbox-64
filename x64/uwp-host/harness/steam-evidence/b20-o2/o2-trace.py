"""B20 O2: build surface-trace-steam.tsv from the routed (THROUGH uwph) evidence."""
import collections, glob, os, re, sys
h = sys.argv[1]; E = f'{h}/out/b20/o2'
runs = [('installer', f'{E}/install', 'SteamSetup.exe /S; WINEDLLPATH=/usr/lib/i386-linux-gnu/wine'),
        ('client-run1', f'{E}/run1-dllpath-i386/client', 'Steam.exe -silent -no-browser; WINEDLLPATH=/usr/lib/i386-linux-gnu/wine'),
        ('client', f'{E}/client2', 'Steam.exe -silent -no-browser; WINEDLLPATH=out/b20/wow64-dlls overlay')]
rows = []
def add(phase, kind, sym, count, ev):
    rows.append((phase, kind, sym, str(count), re.sub(r'\s+', ' ', ev).strip()[:220]))
strip = lambda l: re.sub(r'^\d+\.\d+:[0-9a-f]+:[0-9a-f]+:', '', l)
for phase, base, what in runs:
    lines = open(base + '.log', encoding='utf-8', errors='replace').read().splitlines()
    rc = open(base + ".exit").read().strip()
    add(phase, 'run', what, 1, f'exit={rc}' + (' (180 s cap reached, process still alive)' if rc == '124' else ''))
    groups = collections.OrderedDict()
    def hit(kind, sym, l):
        k = (kind, sym); g = groups.setdefault(k, [0, strip(l)]); g[0] += 1
        if 'load_path' in g[1] and 'load_path' not in l: g[1] = strip(l)  # skip interleaved debug text
    for l in lines:
        if 'winebth' in l: continue
        m = re.search(r'init_peb starting L"(.*)" in experimental wow64 mode', l)
        if m: hit('wow64-in-process', m.group(1).replace('\\\\', '\\').split('\\')[-1], l); continue
        m = re.search(r'NtCreateUserProcess L"[^"]*\\\\([^"\\]+)" pid', l)
        if m: hit('child-process', m.group(1), l); continue
        m = re.search(r'get_builtin_unix_funcs failed to load "([^"]+)"', l)
        if m: hit('missing-unixlib', os.path.basename(m.group(1)), l); continue
        m = re.search(r'Failed to load module L"([^"]+)"; status=(\w+)', l)
        if m: hit('missing-module', m.group(1).replace('\\\\', '\\').split('\\')[-1] + ' ' + m.group(2), l); continue
        m = re.search(r'"([^"]+)" \(ordinal \d+\) not found in L"([^"]+)"', l)
        if m: hit('missing-export', m.group(2).replace('\\\\', '\\').split('\\')[-1] + '!' + m.group(1), l); continue
        m = re.search(r'import_dll (?:No implementation for|Library) (\S+)', l)
        if m: hit('missing-import', m.group(1), l); continue
        m = re.search(r'":?(\w+)" failed to initialize', l)
        if m: hit('dll-init-failure', m.group(1), l); continue
        if ':err:' in l:
            m = re.search(r':err:(\w+:\w+)', l); hit('wine-err', m.group(1), l)
    for (kind, sym), (n, ev) in groups.items(): add(phase, kind, sym, n, ev)
    # Linux-level process model from strace (trace=process)
    ex = collections.Counter()
    for l in open(base + '.exec', errors='replace'):
        m = re.search(r'execve\("([^"]+)".*= 0$', l.strip())
        if m: ex[os.path.basename(m.group(1))] += 1
    for b, n in sorted(ex.items()): add(phase, 'linux-exec', b, n, f'strace execve success; i386 ELF loader execs = {ex.get("wine", 0) + ex.get("wine-preloader", 0)}')
    tot = collections.Counter()
    reps = glob.glob(base + '.counters.*')
    for r in reps:
        for l in open(r):
            p = l.split()
            if p[:1] == ['total']: tot[p[1]] += int(p[2])
    add(phase, 'uwph-routed', 'shim calls ' + ','.join(sorted(tot)), sum(tot.values()),
        f'{len(reps)} per-pid counter reports; ' + ' '.join(f'{k}={v}' for k, v in sorted(tot.items())))
    last = next((strip(l) for l in reversed(lines) if l.strip()), '')
    add(phase, 'last-log-line', 'wine log', 1, last)
blog = f'{h}/out/b20/pfx-wow64-uwph/drive_c/Program Files (x86)/Steam/logs/bootstrap_log.txt'
bl = [l for l in open(blog, errors='replace').read().splitlines() if l.strip()]
for l in bl:
    if 'Error' in l or 'waiting for download' in l: add('client', 'steam-bootstrap', 'bootstrap_log.txt', 1, l)
add('client', 'not-reached', 'steamwebhelper.exe', 0, 'bootstrapper never got its client package (download failed, same under stock Wine control); Chromium multi-process wall unmeasured')
with open(f'{h}/surface-trace-steam.tsv', 'w') as f:
    f.write('phase\tkind\tsymbol_or_surface\tcount\tevidence\n')
    for r in rows: f.write('\t'.join(x.replace('\t', ' ') for x in r) + '\n')
print(len(rows), 'rows')
