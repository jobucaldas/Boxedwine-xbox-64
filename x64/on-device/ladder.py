#!/usr/bin/env python3
"""Hosted diagnostic ladder. No Wine fallback and no credential output."""
import argparse
import os
from pathlib import Path
import signal
import subprocess

STAGES = (
    ('a', 'uwph-smoke'), ('b', 'hello'), ('c', 'hello_k32'),
    ('d', 'wx-jit'), ('e', 'wineserver-in-process'), ('f', 'd3d12-dzn'),
    ('g', 'SteamAPI_Init'), ('h', 'Megabonk'),
)


def execute(command, cwd, timeout):
    # Adapter output may contain private application data: never copy it to logs.
    try:
        child = subprocess.Popen(command, cwd=cwd, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True)
        try:
            rc = child.wait(timeout=timeout)
        except subprocess.TimeoutExpired:
            os.killpg(child.pid, signal.SIGKILL)
            child.wait()
            return 'FAIL', 'timeout'
        if rc == 77:
            return 'SKIP', 'adapter reports unavailable'
        return ('PASS', 'exit=0') if rc == 0 else ('FAIL', f'exit={rc}')
    except OSError:
        return 'FAIL', 'could not execute adapter'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--local-state', type=Path)
    parser.add_argument('--timeout', type=int, default=20)
    args = parser.parse_args()
    if args.timeout < 1 or args.timeout > 60:
        parser.error('--timeout must be 1..60 seconds')
    # out/ladder and source ladder.py both resolve to the same on-device root.
    location = Path(__file__).resolve().parent
    base = location.parent if location.name == 'out' else location
    repo = base.parent.parent
    state = args.local_state or base / 'LocalState'
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (state / 'ladder.log').open('w', encoding='utf-8') as log:
        for letter, name in STAGES:
            adapter = state / 'adapters' / letter
            if letter == 'g' and not (state / 'private' / 'steam.token').is_file():
                status, reason = 'SKIP', 'no token file (contents never read by ladder)'
            elif letter == 'h' and not (state / 'payload' / 'Megabonk.exe').is_file():
                status, reason = 'SKIP', 'payload absent'
            elif letter == 'a' and args.offline:
                status, reason = execute(
                    ['make', '-C', str(repo / 'x64/uwp-host/harness'), 'backend-test'],
                    repo, args.timeout)
                reason += '; THROUGH public uwph Linux backend; not Xbox proof'
            elif args.offline:
                status, reason = 'SKIP', 'offline: device/loader adapter disabled'
            elif not adapter.is_file():
                status, reason = 'SKIP', 'stage adapter unavailable'
            else:
                status, reason = execute([str(adapter.resolve())], state.resolve(), args.timeout)
                reason += '; configured adapter (see routing contract)'
            line = f'STAGE {letter} {name} {status}: {reason}'
            print(line, flush=True)
            log.write(line + '\n')
            log.flush()
    # Diagnostic collection completes despite individual failures/skips.
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
