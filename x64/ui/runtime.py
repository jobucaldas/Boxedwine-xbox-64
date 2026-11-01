"""Linux-only owned-fixture adapter; no Steam traffic or arbitrary launches."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from dataclasses import dataclass
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
OUT = Path(__file__).resolve().parent / 'out'


@dataclass(frozen=True)
class Result:
    status: str
    exit_code: int
    output: str
    child_exit_code: int | None = None
    log_path: str | None = None


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(command, cwd, timeout=180, terminate_grace=2):
    """Own the Linux worker session, including cleanup after a timeout.

    Keep the pipe open until group cleanup finishes so late cleanup diagnostics
    are retained. Kill the group even if make exits during the TERM grace:
    a descendant can ignore TERM or close its inherited output descriptors.
    """
    with subprocess.Popen(command, cwd=cwd, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT, text=True,
                          start_new_session=True) as child:
        try:
            output, _ = child.communicate(timeout=timeout)
            return subprocess.CompletedProcess(command, child.returncode, output)
        except subprocess.TimeoutExpired:
            def signal_group(sig):
                try:
                    os.killpg(child.pid, sig)
                except ProcessLookupError:
                    pass

            signal_group(signal.SIGTERM)
            try:
                output, _ = child.communicate(timeout=terminate_grace)
            except subprocess.TimeoutExpired:
                output = None
            finally:
                signal_group(signal.SIGKILL)
            # Reap the direct child and drain late output after escalation.
            # Bound pipe draining too: a detached process must not keep this
            # worker blocked indefinitely by retaining an inherited pipe.
            if output is None:
                try:
                    output, _ = child.communicate(timeout=terminate_grace)
                except subprocess.TimeoutExpired as error:
                    output = error.output or b''
                    if isinstance(output, bytes):
                        output = output.decode(errors='replace')
                    child.stdout.close()
                    child.wait(timeout=terminate_grace)
            return subprocess.CompletedProcess(
                command, 124, output + '\nRUNTIME ERROR: worker timed out\n')


class Runtime:
    def __init__(self, root=ROOT, out=OUT, runner=run):
        self.root, self.out, self.runner = Path(root), Path(out), runner

    def install(self, game):
        if game != 'hello-demo':
            return Result('unavailable', 3, 'INSTALL UNAVAILABLE: owned hello-demo only')
        self.out.mkdir(parents=True, exist_ok=True)
        # build.sh runs unchanged in isolation, never overwriting tracked PEs.
        try:
            with tempfile.TemporaryDirectory(dir=self.out, prefix='build-') as work:
                work = Path(work)
                for name in ('build.sh', 'hello.c', 'hello_k32.c'):
                    shutil.copy2(self.root / 'x64/pe/tests' / name, work / name)
                child = self.runner(['sh', 'build.sh'], work)
                (self.out / 'install.log').write_text(child.stdout)
                if child.returncode:
                    return Result('error', 1, 'INSTALL ERROR: fixture build failed\n' + child.stdout,
                                  child.returncode, str(self.out / 'install.log'))
                fixture = self.out / 'hello.exe'
                shutil.copy2(work / 'hello.exe', fixture)
                state = {'game': game, 'ready': True, 'path': str(fixture.resolve()),
                         'sha256': digest(fixture), 'mock_only': True,
                         'harness_fixtures': {name: digest(self.root / 'x64/pe/tests' / name)
                                              for name in ('hello.exe', 'hello_k32.exe')}}
                (self.out / 'hello-demo.json').write_text(json.dumps(state, indent=2) + '\n')
            return Result('ready', 0, f'MOCK INSTALL READY: {game}\n{fixture.resolve()} sha256={state["sha256"]}\nNo Steam content was downloaded. Launch runs the existing harness batch fixtures.')
        except (OSError, subprocess.SubprocessError) as error:
            return Result('error', 1, f'INSTALL ERROR: {error}')

    def launch(self, game):
        """Submit this blocking method to a UI worker, or use launch_async."""
        if game != 'hello-demo':
            return Result('unavailable', 3, 'LAUNCH UNAVAILABLE: owned hello-demo only')
        state_path = self.out / 'hello-demo.json'
        if not state_path.exists():
            return Result('not_installed', 3, 'LAUNCH NOT INSTALLED: hello-demo')
        try:
            state = json.loads(state_path.read_text())
            fixture = self.out / 'hello.exe'
            if (state['game'] != game or state['ready'] is not True or
                    state['path'] != str(fixture.resolve()) or digest(fixture) != state['sha256']):
                raise ValueError('installed fixture readiness/hash mismatch')
            for name in ('hello.exe', 'hello_k32.exe'):
                if digest(self.root / 'x64/pe/tests' / name) != state['harness_fixtures'][name]:
                    raise ValueError('harness fixture changed; install again')
            self.out.mkdir(parents=True, exist_ok=True)
            evidence = Path(tempfile.mkdtemp(dir=self.out, prefix='launch-'))
            child = self.runner(['make', '-C', 'x64/uwp-host/harness', 'wine-test'], self.root)
            raw = child.stdout
            log = evidence / 'child.log'
            log.write_text(raw)
            (evidence / 'child.exit').write_text(str(child.returncode) + '\n')
            # Only the unchanged verifier's exact wineboot-only rejection is known.
            blockers = re.findall(r'^Wine uwph verification: BLOCKED: (.+)$', raw, re.M)
            known = (child.returncode == 2 and len(blockers) == 1 and
                     all(re.fullmatch(r'(control|routed)-(hello|hello_k32): wineboot activity/attempt in trace', item)
                         for item in blockers[0].split('; ')))
            if child.returncode == 0 and 'Wine uwph verification: PASS' in raw:
                status, code, message = 'verified', 0, 'LAUNCH VERIFIED: selected Linux surface only'
            elif known:
                status, code, message = 'blocked', 2, 'LAUNCH BLOCKED: wineboot attempts rejected by unchanged R4H gate'
            else:
                status, code, message = 'error', 1, f'LAUNCH ERROR: harness failure (child exit={child.returncode})'
            output = ('HARNESS BATCH: hello.exe + hello_k32.exe, routed runs and stock controls; '
                      'not a single installed game launch.\n' + raw +
                      f'\nRaw child log: {log}; child exit={child.returncode}\n' + message)
            (evidence / 'result.json').write_text(json.dumps({'status': status, 'exit_code': code,
                                                            'child_exit_code': child.returncode}, indent=2))
            return Result(status, code, output, child.returncode, str(log))
        except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError) as error:
            return Result('error', 1, f'LAUNCH ERROR: {error}')

    def launch_async(self, game, executor):
        return executor.submit(self.launch, game)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('action', choices=('install', 'launch'))
    parser.add_argument('--game', required=True)
    args = parser.parse_args()
    runtime = Runtime()
    if args.action == 'launch':
        with ThreadPoolExecutor(max_workers=1) as worker:
            result = runtime.launch_async(args.game, worker).result()
    else:
        result = runtime.install(args.game)
    print(result.output)
    return result.exit_code


if __name__ == '__main__':
    raise SystemExit(main())
