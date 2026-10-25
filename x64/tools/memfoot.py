"""1 Hz aggregate RSS and Linux accountable mapping commitment estimate.

smaps_rollup has no commit field. Commit here sums Size of smaps mappings
with VmFlags ac (Linux accountable virtual allocations), not VmSize, RSS,
or Windows/UWP commit. Shared RSS can be counted more than once. Short-lived
processes between ticks can be missed: all peaks are sampled lower bounds.
"""
import csv
import os
from pathlib import Path
import signal
import subprocess
import sys
import time
import fcntl
from datetime import datetime, timezone

root = Path(sys.argv[1])
h = root / 'x64/uwp-host/harness'
out = h / 'out/memfoot'
out.mkdir(parents=True, exist_ok=True)
lock = (out / 'lock').open('w')
fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
prefix = out / 'prefix'
out = out / (datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ') + f'-{os.getpid()}')
out.mkdir()


def interrupted(signum, frame):
    raise SystemExit(128 + signum)


for sig in (signal.SIGTERM, signal.SIGHUP):
    signal.signal(sig, interrupted)
if not prefix.exists():
    subprocess.run(['cp', '-a', '--reflink=auto', '/workspace/megabonk/pfx-stock', str(prefix)], check=True)
base = dict(os.environ, WINEPREFIX=str(prefix), WINEDLLOVERRIDES='wineboot.exe=d', WINEDEBUG='-all')
base.pop('LD_PRELOAD', None)
base.pop('UWPH_REPORT', None)


def processes(*sessions):
    # Wine helpers can call setsid; the private prefix also identifies them.
    marker = b'WINEPREFIX=' + os.fsencode(prefix) + b'\0'
    for p in Path('/proc').glob('[0-9]*'):
        try:
            if os.getsid(int(p.name)) in sessions or marker in (p / 'environ').read_bytes():
                yield p
        except (ProcessLookupError, FileNotFoundError, PermissionError):
            pass


def memory(p):
    roll = dict((line.split(':')[0], int(line.split()[1]))
                for line in (p / 'smaps_rollup').read_text().splitlines()[1:] if ':' in line)
    commit = size = 0
    for line in (p / 'smaps').read_text().splitlines():
        if line.startswith('Size:'):
            size = int(line.split()[1])
        elif line.startswith('VmFlags:') and 'ac' in line.split()[1:]:
            commit += size
    return roll['Rss'], commit


stages = [('hello', root / 'x64/pe/tests/hello.exe', 15),
          ('hello_k32', root / 'x64/pe/tests/hello_k32.exe', 15),
          ('Megabonk', Path('/workspace/steam-owned/games/megabonk/Megabonk.exe'), 120)]
# B10 trace marks the merged integration; no credentials are accessed.
if (h / 'surface-trace-steam.tsv').exists():
    steam = os.environ.get('MEMFOOT_STEAM_EXE')
    if not steam or not Path(steam).is_file():
        raise SystemExit('B10 present: set MEMFOOT_STEAM_EXE to the real installed Steam client')
    stages.insert(2, ('Steam', Path(steam).resolve(), 180))
rows = []
consumers = []
with (out / 'samples.tsv').open('w') as raw:
    raw.write('stage\telapsed_s\tpid\tname\trss_kb\tcommit_estimate_kb\n')
    for stage, exe, seconds in stages:
        for stale in out.glob(stage + '.counters*'):
            stale.unlink()
        env = dict(base, WINESERVER='/bin/false', WINE_LOADER=str(h / 'out/wine-wx/lib/wine/wine64'),
                   LD_PRELOAD=str(h / 'out/libuwph_wine_shim.so'), UWPH_REPORT_PID='1',
                   UWPH_REPORT=str(out / (stage + '.counters')))
        # Session includes wineserver AND launcher children, including reparented
        # helpers. Server starts without preload, just as in run-wine.sh.
        with (out / (stage + '.log')).open('w') as log:
            server = subprocess.Popen(['/usr/lib/wine/wineserver64', '-f', '-p'], env=base,
                                      start_new_session=True, stdout=log, stderr=log)
            # Join server session is not possible from here; independently track
            # both sessions, including all server-spawned helpers.
            client = None
            peak_rss = peak_commit = ticks = errors = 0
            max_gap = 0.0
            previous_tick = None
            individual = {}
            start = time.monotonic()
            try:
                time.sleep(.2)
                if server.poll() is not None:
                    raise SystemExit(f'{stage}: private wineserver exited before client launch; '
                                     f"prefix may already be owned; see {out / (stage + '.log')}")
                client = subprocess.Popen([str(h / 'out/run_via_wine'), str(exe)], env=env,
                                          cwd=exe.parent, start_new_session=True, stdout=log, stderr=log)
                deadline = time.monotonic() + seconds
                while True:
                    tick_start = time.monotonic()
                    if previous_tick is not None:
                        max_gap = max(max_gap, tick_start - previous_tick)
                    previous_tick = tick_start
                    rss = commit = 0
                    for p in processes(server.pid, client.pid):
                        try:
                            r, c = memory(p)
                            name = (p / 'comm').read_text().strip().replace('\t', ' ')
                        except FileNotFoundError:
                            continue
                        except (PermissionError, ProcessLookupError):
                            errors += 1
                            continue
                        rss += r
                        commit += c
                        key = (p.name, name)
                        individual[key] = max(individual.get(key, 0), r)
                        raw.write(f'{stage}\t{time.monotonic()-start:.3f}\t{p.name}\t{name}\t{r}\t{c}\n')
                    raw.flush()
                    ticks += 1
                    peak_rss = max(peak_rss, rss)
                    peak_commit = max(peak_commit, commit)
                    if client.poll() is not None or time.monotonic() >= deadline:
                        break
                    time.sleep(max(0, min(tick_start + 1, deadline) - time.monotonic()))
                elapsed = time.monotonic() - start
                max_gap = max(max_gap, time.monotonic() - previous_tick)
                rc = client.poll()
                status = 'timeout' if rc is None else f'exit_{rc}'
                reports = list(out.glob(stage + '.counters.*'))
                routed = any('total mprotect ' in p.read_text() for p in reports)
                peak = max(peak_rss, peak_commit) / 1024
                rows.append([stage, f'{peak_rss/1024:.2f}', f'{peak_commit/1024:.2f}', f'{peak:.2f}',
                             'yes' if peak <= 1024 else 'no', 'yes' if peak <= 5120 else 'no',
                             status, ticks, errors, 'observed' if routed else 'unconfirmed',
                             f'{elapsed:.3f}', f'{max_gap:.3f}',
                             'delayed' if max_gap > 2 else 'ok', str(out)])
                consumers.extend((r, stage, pid, name) for (pid, name), r in individual.items())
            finally:
                # Let cleanup finish even if the driver repeats its termination signal.
                for sig in (signal.SIGTERM, signal.SIGHUP):
                    signal.signal(sig, signal.SIG_IGN)
                for proc in (client, server):
                    if proc is not None:
                        try:
                            os.killpg(proc.pid, signal.SIGTERM)
                        except ProcessLookupError:
                            pass
                for proc in (client, server):
                    if proc is not None:
                        try:
                            proc.wait(timeout=2)
                        except subprocess.TimeoutExpired:
                            pass
                        # The session leader can exit before its helpers do.
                        try:
                            os.killpg(proc.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                        proc.wait()
                for p in processes():
                    try:
                        os.kill(int(p.name), signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                for sig in (signal.SIGTERM, signal.SIGHUP):
                    signal.signal(sig, interrupted)
with (root / 'x64/tools/memfoot.tsv').open('w') as f:
    w = csv.writer(f, delimiter='\t', lineterminator='\n')
    w.writerow(['stage', 'peak_rss_mb', 'peak_commit_estimate_mb', 'peak_mb', 'fits_app', 'fits_game',
                'status', 'samples', 'read_errors', 'routing', 'elapsed_s', 'max_gap_s',
                'cadence', 'evidence_dir'])
    w.writerows(rows)
(out / 'summary.tsv').write_text((root / 'x64/tools/memfoot.tsv').read_text())
with (out / 'top.tsv').open('w') as f:
    f.write('rss_mb\tstage\tpid\tname\n')
    for r, stage, pid, name in sorted(consumers, reverse=True)[:3]:
        f.write(f'{r/1024:.2f}\t{stage}\t{pid}\t{name}\n')
print('THROUGH launcher -> Wine W^X loader -> selected uwph calls; separate stock wineserver included.')
print('Sampled Linux estimates only; commit = accountable mapping Size, not UWP commit. Steam conditional on merged B10 trace.')
print('Evidence:', out)
