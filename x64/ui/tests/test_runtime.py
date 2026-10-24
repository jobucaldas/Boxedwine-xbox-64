"""Injected process results are behavior checks, never live routing proof."""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import threading
import time
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import Runtime, run


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        sources = self.root / 'x64/pe/tests'
        sources.mkdir(parents=True)
        for name in ('build.sh', 'hello.c', 'hello_k32.c', 'hello.exe', 'hello_k32.exe'):
            (sources / name).write_bytes(b'owned original')
        self.response = subprocess.CompletedProcess([], 2, 'Wine uwph verification: BLOCKED: routed-hello: wineboot activity/attempt in trace\n')
        self.commands = []

        def runner(command, cwd):
            self.commands.append(command)
            if command == ['sh', 'build.sh']:
                (cwd / 'hello.exe').write_bytes(b'owned build')
                return subprocess.CompletedProcess(command, 0, 'build ok\n')
            return self.response
        self.runtime = Runtime(self.root, self.root / 'out', runner)

    def test_unavailable_and_not_installed(self):
        self.assertEqual(self.runtime.install('other').status, 'unavailable')
        self.assertEqual(self.runtime.launch('other').status, 'unavailable')
        self.assertEqual(self.runtime.launch('hello-demo').status, 'not_installed')
        self.assertEqual(self.commands, [])

    def test_install_is_owned_and_isolated(self):
        result = self.runtime.install('hello-demo')
        self.assertEqual(result.exit_code, 0)
        self.assertIn('No Steam content was downloaded', result.output)
        self.assertIn('sha256=', result.output)
        self.assertEqual((self.root / 'x64/pe/tests/hello.exe').read_bytes(), b'owned original')

    def test_known_blocker_and_raw_status(self):
        self.runtime.install('hello-demo')
        result = self.runtime.launch('hello-demo')
        self.assertEqual((result.status, result.exit_code, result.child_exit_code), ('blocked', 2, 2))
        self.assertEqual(Path(result.log_path).read_text(), self.response.stdout)
        self.assertEqual(self.commands[-1], ['make', '-C', 'x64/uwp-host/harness', 'wine-test'])

    def test_unrelated_failure_is_error(self):
        self.runtime.install('hello-demo')
        for code, text in ((2, 'Missing initialized fixture'), (2, 'Wine uwph verification: BLOCKED: missing handoff\n'), (0, 'no verification'), (127, 'missing make')):
            self.response = subprocess.CompletedProcess([], code, text)
            result = self.runtime.launch('hello-demo')
            self.assertEqual(result.status, 'error')
            self.assertEqual(result.child_exit_code, code)

    def test_verified_and_tampered_fixture(self):
        self.runtime.install('hello-demo')
        self.response = subprocess.CompletedProcess([], 0, 'Wine uwph verification: PASS\n')
        self.assertEqual(self.runtime.launch('hello-demo').status, 'verified')
        (self.runtime.out / 'hello.exe').write_bytes(b'changed')
        self.assertEqual(self.runtime.launch('hello-demo').status, 'error')

    def test_missing_prerequisite(self):
        self.runtime.install('hello-demo')
        def missing(command, cwd):
            raise FileNotFoundError('make unavailable')
        self.runtime.runner = missing
        self.assertEqual(self.runtime.launch('hello-demo').status, 'error')

    def test_timeout_cleans_real_descendant_and_preserves_evidence(self):
        self.runtime.install('hello-demo')
        marker = self.root / 'escaped-marker'
        pidfile = self.root / 'descendant.pid'
        # The parent reaps its descendant on TERM, just as a cooperating
        # harness shell can run its EXIT cleanup during the grace interval.
        script = """
import pathlib, signal, subprocess, sys, time
child = subprocess.Popen([sys.executable, '-c',
    'import pathlib,time,sys; time.sleep(1); pathlib.Path(sys.argv[1]).touch()',
    str(sys.argv[1])])
def cleanup(sig, frame):
    child.wait()
    print('parent cleanup complete', flush=True)
    sys.exit(0)
signal.signal(signal.SIGTERM, cleanup)
pathlib.Path(sys.argv[2]).write_text(str(child.pid))
print('worker output before timeout', flush=True)
while True: time.sleep(1)
"""
        def real_runner(command, cwd):
            return run([sys.executable, '-c', script, str(marker), str(pidfile)],
                       cwd, timeout=0.4, terminate_grace=0.5)
        self.runtime.runner = real_runner
        started = time.monotonic()
        result = self.runtime.launch('hello-demo')
        self.assertLess(time.monotonic() - started, 3)
        self.assertEqual((result.status, result.exit_code, result.child_exit_code),
                         ('error', 1, 124))
        raw = Path(result.log_path).read_text()
        self.assertIn('worker output before timeout', raw)
        self.assertIn('parent cleanup complete', raw)
        self.assertIn('worker timed out', raw)
        self.assertEqual(Path(result.log_path).with_name('child.exit').read_text(), '124\n')
        with self.assertRaises(ProcessLookupError):
            os.kill(int(pidfile.read_text()), 0)
        time.sleep(1.1)
        self.assertFalse(marker.exists())
        # An uncooperative direct worker must also be killed within the bound.
        killed = run([sys.executable, '-c',
                      "import signal,time; signal.signal(signal.SIGTERM, signal.SIG_IGN); print('stubborn', flush=True); time.sleep(30)"],
                     self.root, timeout=0.3, terminate_grace=0.1)
        self.assertEqual(killed.returncode, 124)
        self.assertIn('stubborn', killed.stdout)

    def test_async_submission_returns_pending_future_and_propagates_result(self):
        self.runtime.install('hello-demo')
        entered, release = threading.Event(), threading.Event()
        def waiting_runner(command, cwd):
            entered.set()
            if not release.wait(3):
                raise RuntimeError('test worker was not released')
            return self.response
        self.runtime.runner = waiting_runner
        with ThreadPoolExecutor(max_workers=1) as executor:
            future = self.runtime.launch_async('hello-demo', executor)
            try:
                self.assertTrue(entered.wait(1))
                self.assertFalse(future.done())
            finally:
                release.set()
            result = future.result(timeout=2)
        self.assertEqual((result.status, result.exit_code, result.child_exit_code),
                         ('blocked', 2, 2))
        self.assertEqual(Path(result.log_path).read_text(), self.response.stdout)


if __name__ == '__main__':
    unittest.main()
