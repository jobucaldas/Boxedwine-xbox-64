#!/usr/bin/env python3
"""Fixture tests for verify_wine.py --inproc (no Wine run; fake run dirs only).

Each fixture is a run dir that satisfies every verify_wine.py check except the
knobs under test: the routed execv count, the [inproc] banner, and a wineserver
execve in the exec trace.
"""
import hashlib
import json
import os
import pathlib
import subprocess
import sys
import tempfile

HERE = pathlib.Path(__file__).resolve().parent
VERIFY = HERE / 'verify_wine.py'
SHIM = (HERE / 'out/libuwph_wine_shim.so').resolve()
SELECTED = ('open', 'read', 'close', 'mmap', 'munmap', 'mprotect')
BANNER = '[inproc] Wine loader pid=5 server thread ready; no separate server process\n'
PID = 5


def make_run(root, execv, banner=True, wineserver_exec=False):
    run = root / 'run'
    run.mkdir()
    ntdlls = {}
    for mode, file in (('control', 'identity.json'), ('routed', 'identity-wx.json')):
        ntdll = root / mode / 'ntdll.so'
        ntdll.parent.mkdir()
        ntdll.write_bytes(f'fake {mode} ntdll\n'.encode())
        ntdlls[mode] = ntdll.resolve()
        sha = hashlib.sha256(ntdll.read_bytes()).hexdigest()
        (run / file).write_text(json.dumps({'artifacts': {'unix_ntdll': {'path': str(ntdll), 'sha256': sha}}}))
    rows = [f'{op}\trouted' for op in SELECTED] + ['execv\trouted', 'write\trouted', 'fork\tunsupported']
    (run / 'surface.tsv').write_text('symbol\tclass\n' + '\n'.join(rows) + '\n')
    for mode in ('control', 'routed'):
        ntdll = ntdlls[mode]
        for pe in ('hello_k32', 'hello'):
            name = f'{mode}-{pe}'
            log = (f"[launcher] exec /fake/wine64 -> /fake/{pe}.exe (handing guest to Wine's loader)\n"
                   f'0024:trace:loaddll:build_module Loaded L"Z:\\\\fake\\\\{pe}.exe" at 0000000140000000: native\n')
            if banner:
                log += BANNER
            if pe == 'hello_k32':
                log += 'hello from a native x86-64 PE (kernel32 only)\n'
            else:
                log += ('hello.exe: native x86-64 Windows PE, argc=1\nTEB self-check: ok, sizeof(void*)=8\n'
                        'malloc: heap, 64-bit math\nthreads: shared=200000 exit codes=6,7 main tls=5\n')
            trace = f'{PID}     execve("/fake/out/run_via_wine", ["/fake/out/run_via_wine", "/fake/{pe}.exe"], 0x0 /* 1 vars */) = 0\n'
            if wineserver_exec:
                trace += f'{PID + 1}     execve("/usr/lib/wine/wineserver64", ["/usr/lib/wine/wineserver64"], 0x0 /* 1 vars */) = 0\n'
            trace += f'{PID}     +++ exited with 0 +++\n'
            if mode == 'routed':
                for op in SELECTED:
                    log += f"     {PID}:\tbinding file {ntdll} [0] to {SHIM} [0]: normal symbol `{op}'\n"
                report = [f'pid {PID}']
                report += [f'total {op} 7 0' for op in SELECTED]
                report += [f'module {str(ntdll).encode().hex()} {op} 7 7' for op in SELECTED]
                report += [f'trace execv {execv}', 'trace write 3', 'trace mprotect 7']
                report += ['classification recursion=0 unattributed=0 module_overflow=0 instrumentation_raw_io=1']
                (run / f'{name}.report.{PID}').write_text('\n'.join(report) + '\n')
            (run / f'{name}.log').write_text(log)
            (run / f'{name}.exec').write_text(trace)
            (run / f'{name}.exit').write_text('0\n')
    return run


def verify(run, flag=False, env_inproc=False):
    env = {k: v for k, v in os.environ.items() if k != 'UWPH_INPROC_SERVER'}
    if env_inproc:
        env['UWPH_INPROC_SERVER'] = '1'
    cmd = [sys.executable, str(VERIFY)] + (['--inproc'] if flag else []) + [str(run)]
    proc = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=60)
    return proc.returncode, proc.stdout + proc.stderr


# (label, fixture kwargs, verify kwargs, expected rc, required output substring)
CASES = [
    ('inproc: execv=0 + banner -> PASS', dict(execv=0), dict(flag=True), 0,
     'Wine uwph verification: PASS'),
    ('inproc: execv>0 -> FAIL', dict(execv=1), dict(flag=True), 1,
     'BLOCKED: routed-hello_k32: in-process mode requires exactly 0 uwph execv entries, got 1'),
    ('inproc: banner missing -> FAIL', dict(execv=0, banner=False), dict(flag=True), 1,
     'BLOCKED: routed-hello_k32: in-process mode missing [inproc] no-separate-server banner'),
    ('default: execv=0 -> FAIL as today', dict(execv=0), dict(), 1,
     'BLOCKED: routed-hello_k32: O2 traced symbol with zero uwph entries'),
    # Extra coverage: env switch, wineserver execve, and fixture validity in default mode.
    ('inproc via UWPH_INPROC_SERVER=1: execv=0 + banner -> PASS', dict(execv=0), dict(env_inproc=True), 0,
     'Wine uwph verification: PASS'),
    ('inproc: wineserver execve in trace -> FAIL', dict(execv=0, wineserver_exec=True), dict(flag=True), 1,
     'BLOCKED: control-hello_k32: in-process mode but wineserver execve in exec trace'),
    ('default: execv>0 -> PASS (fixture sanity)', dict(execv=1), dict(), 0,
     'Wine uwph verification: PASS'),
]


def main():
    failed = 0
    for label, fixture, mode, want_rc, want_text in CASES:
        with tempfile.TemporaryDirectory(prefix='verify-wine-inproc-') as tmp:
            rc, out = verify(make_run(pathlib.Path(tmp), **fixture), **mode)
        ok = rc == want_rc and want_text in out
        failed += not ok
        print(f"{'ok  ' if ok else 'FAIL'} {label} (rc={rc})")
        if not ok:
            print('     expected rc=%d and %r; output:\n%s' % (want_rc, want_text, out))
    print(f'{len(CASES) - failed} passed, {failed} failed')
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
