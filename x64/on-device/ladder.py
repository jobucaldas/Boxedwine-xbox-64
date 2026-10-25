#!/usr/bin/env python3
"""Hosted diagnostic ladder. No Wine fallback and no credential output."""
import argparse
import os
from pathlib import Path
import signal
import subprocess
import tempfile

STAGES = (
    ('a', 'uwph-smoke'), ('b', 'hello'), ('c', 'hello_k32'),
    ('d', 'wx-jit'), ('e', 'wineserver-in-process'), ('f', 'd3d12-dzn'),
    ('g', 'SteamAPI_Init'), ('h', 'Megabonk'), ('s1', 'steam-download'),
    ('combo', 'hello+hello_k32-nofork,noproc,wx'),
)


def execute(command, cwd, timeout, env=None):
    # Adapter output may contain private application data: never copy it to logs.
    try:
        child = subprocess.Popen(command, cwd=cwd, stdout=subprocess.DEVNULL,
                                 stderr=subprocess.DEVNULL, start_new_session=True, env=env)
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


STANDIN = (('n10', 'platform-allow'), ('n11', 'loopback-dlls'))


def standin(base, timeout):
    """Only the packaged, fixed n10/n11 probes may supply public probe sub-lines."""
    payload = base / 'payload'
    if not (payload / 'rungs.exe').is_file():
        payload = base / 'out/payload'
    launcher = payload / 'combo/run_via_wine'
    if not launcher.is_file() or not Path('/usr/lib/wine/wine64').is_file():
        return {}, {r: 'standin unavailable: launcher/payload or desktop Wine missing' for r, _ in STANDIN}
    with tempfile.TemporaryDirectory(prefix='n1x-standin-') as temp:
        root = Path(temp)
        env = os.environ.copy()
        env.update(WINEPREFIX=str(root / 'prefix'), WINEDEBUG='-all',
                   WINEDLLOVERRIDES='mscoree,mshtml=', WINE_LOADER='/usr/lib/wine/wine64')
        state = root / 'state'
        state.mkdir()
        try:
            status, reason = execute(
                [str(launcher.resolve()), str((payload / 'rungs.exe').resolve())]
                + [f'--{r}-standin' for r, _ in STANDIN]
                + ['--local-state', 'Z:' + str(state)], root, timeout, env=env)
            # Read only our bounded n10/n11 public diagnostics, never adapter stdout.
            evidence = state / 'ladder-native.log'
            log = evidence.read_text(errors='replace').splitlines() if evidence.is_file() else []
            lines, reasons = {}, {}
            for r, _ in STANDIN:
                lines[r] = [line for line in log if line.startswith(f'# {r} | standin ')]
                # Never echo the adapter verdict word: a hosted line must not read as PASS.
                collected = 'collected' if status == 'PASS' and lines[r] else f'not collected ({reason})'
                reasons[r] = f'standin {collected}; THROUGH our launcher -> stock Wine loader; no uwph or Xbox proof'
            return lines, reasons
        finally:
            try:
                subprocess.run(['/usr/lib/wine/wineserver', '-k'], env=env,
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=5)
            except (OSError, subprocess.TimeoutExpired):
                pass


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--offline', action='store_true')
    parser.add_argument('--dry-run', action='store_true', help='list s1 console plan without executing or accessing credentials/network')
    parser.add_argument('--big-file', action='store_true', help='opt in to native >2 GB storage rung')
    parser.add_argument('--local-state', type=Path)
    parser.add_argument('--timeout', type=int, default=20)
    args = parser.parse_args()
    if args.timeout < 1 or args.timeout > 60:
        parser.error('--timeout must be 1..60 seconds')
    if args.dry_run:
        for step in (
            'foreground: keep screen on; Game mode has no background downloads (DOCS-ANSWERS row 19)',
            'anonymous tile: download app 1007 into fresh LocalState/steam-downloads/1007 via steampipe.akamaized.net',
            'integrity: verify chunk and file SHA-1, reopen files and verify size/hash; log bytes/s to LocalState/steam-dl.log',
            'optional separate tile: display QR on TV; Joao scans and approves on phone; hold login only in memory, never copy a token to console',
            'owned: download app 3405340 only when owned; verify SHA-1 and log bytes/s; no launch or DRM modification',
            'BLOCKED: B53 desktop WinHTTP transport needs documented in-process UWP transport and QR rendering before either tile can run',
        ):
            print('STEP s1 ' + step)
        return 0
    # out/ladder and source ladder.py both resolve to the same on-device root.
    location = Path(__file__).resolve().parent
    base = location.parent if location.name == 'out' else location
    repo = base.parent.parent
    state = args.local_state or base / 'LocalState'
    state.mkdir(parents=True, exist_ok=True, mode=0o700)
    with (state / 'ladder.log').open('w', encoding='utf-8') as log:
        for letter, name in STAGES:
            adapter = state / 'adapters' / letter
            if letter == 's1':
                status, reason = 'SKIP', 'B53 kit diagnostic only; UWP transport, in-process integration and QR tiles pending; no console download proof'
            elif letter == 'g' and not (state / 'private' / 'steam.token').is_file():
                status, reason = 'SKIP', 'no token file (contents never read by ladder)'
            elif letter == 'h' and not (state / 'payload' / 'Megabonk.exe').is_file():
                status, reason = 'SKIP', 'payload absent'
            elif letter == 'a' and args.offline:
                status, reason = execute(
                    ['make', '-C', str(repo / 'x64/uwp-host/harness'), 'backend-test'],
                    repo, args.timeout)
                reason += '; THROUGH public uwph Linux backend; not Xbox proof'
            elif letter == 'combo' and args.offline:
                status, reason = execute(
                    [str((base if (base / 'payload/combo/run').is_file()
                          else base / 'out') / 'payload/combo/run')], location, 60)
                reason += '; nofork,noproc,wx THROUGH launcher -> bundled W^X Wine loader -> uwph; Linux only'
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
        native = [(f'p{i}', name) for i, name in enumerate(
            ('seh_probe', 'thread_probe', 'fs_probe', 'net_probe', 'jit_probe', 'uwph-self-test'), 1)] + [('n0', 'probe-native')] + [
            ('n1', 'd3d11-device'), ('n2', 'd3d12-device'),
            ('n3', 'corewindow-swapchain'), ('n4', 'audio'),
            ('n5', 'game-memory'), ('n6', 'gaming-device-model'), ('n7', 'big-file'), ('n8', 'gamepad')]
        for ident, name in native:
            if args.offline:
                status, reason = 'SKIP', 'offline: native console APIs require UWP activation; no Xbox proof'
            elif ident.startswith('p') or ident == 'n8':
                status, reason = 'SKIP', 'run packaged rungs.exe with verified Xbox UWP activation; hosted adapters cannot attest readiness'
            elif ident == 'n7' and not args.big_file:
                status, reason = 'SKIP', 'requires --big-file'
            else:
                adapter = state / 'adapters' / ident
                if adapter.is_file():
                    command = [str(adapter.resolve())]
                    if ident == 'n7':
                        command.append('--big-file')
                    status, reason = execute(command, state.resolve(), args.timeout)
                    reason += '; configured native adapter; verify console provenance'
                else:
                    status, reason = 'SKIP', 'native adapter unavailable; launch packaged rungs.exe on console'
            line = f'STAGE {ident} {name} {status}: {reason}'
            print(line, flush=True)
            log.write(line + '\n')
            log.flush()
        lines, reasons = standin(base, 60)  # first run may create a Wine prefix
        for ident, name in STANDIN:
            for line in lines.get(ident, []):
                print(line, flush=True)
                log.write(line + '\n')
            line = f'STAGE {ident} {name} SKIP: {reasons[ident]}; console verdict=SKIP'
            print(line, flush=True)
            log.write(line + '\n')
            log.flush()
    # Diagnostic collection completes despite individual failures/skips.
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
