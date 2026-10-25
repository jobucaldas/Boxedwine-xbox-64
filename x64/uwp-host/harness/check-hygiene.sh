#!/usr/bin/env bash
# One hello THROUGH our launcher, Wine loader and uwph, with prefix cleanup.
set -euo pipefail
cd "$(dirname "$0")"
make out/run_hello
python3 - <<'PY'
import os, pathlib, subprocess
out = pathlib.Path('out')
def runs():
    return {str(pathlib.Path(base) / d) for base, dirs, _ in os.walk(out, followlinks=False)
            for d in dirs if d.startswith('run-') and not (pathlib.Path(base) / d).is_symlink()}
def check_z():
    for base, dirs, _ in os.walk(out, followlinks=False):
        for d in dirs:
            p = pathlib.Path(base) / d
            if d == 'z:' and p.parent.name == 'dosdevices' and not p.is_symlink():
                raise SystemExit(f'FAIL real dosdevices/z: directory: {p}')
check_z()
before = runs()
env = dict(os.environ, KEEP_RUN='0')
with (out / 'hygiene-hello.log').open('w') as log:
    rc = subprocess.call(['./out/run_hello'], env=env, stdout=log, stderr=subprocess.STDOUT)
print((out / 'hygiene-hello.log').read_text(), end='')
check_z()
assert not runs() - before, f'FAIL new run directories remain: {runs() - before}'
assert rc == 0, f'FAIL hello exited {rc}'
print('PASS hygiene: one hello THROUGH launcher -> Wine loader -> uwph; no new run-* or real dosdevices/z:')
PY
