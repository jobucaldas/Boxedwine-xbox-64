"""Injected process results are behavior checks, never live routing proof."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from runtime import Runtime


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


if __name__ == '__main__':
    unittest.main()
