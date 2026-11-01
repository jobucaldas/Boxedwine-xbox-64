import sys
from pathlib import Path
import threading
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from app import Shell, wait_action
from runtime import Result


class FakeRuntime:
    def __init__(self):
        self.calls = []
        self.release = threading.Event()

    def install(self, game):
        self.calls.append(('install', game))
        return Result('ready', 0, 'MOCK INSTALL READY: ' + game)

    def launch_async(self, game, executor):
        return executor.submit(self.launch, game)

    def launch(self, game):
        self.calls.append(('launch', game))
        self.release.wait(2)
        return Result('blocked', 2, 'LAUNCH BLOCKED')


class UIFlowTests(unittest.TestCase):
    def setUp(self):
        self.runtime = FakeRuntime()
        self.shell = Shell(self.runtime)

    def tearDown(self):
        self.runtime.release.set()
        self.shell.close()

    def test_login_navigation_and_back(self):
        self.shell.dispatch('activate')
        self.assertEqual(len(self.shell.games), 3)
        self.shell.dispatch('right')
        self.assertEqual(self.shell.games[self.shell.selected].id, 'mock-orbit')
        self.shell.dispatch('down')
        self.shell.dispatch('right')
        self.shell.dispatch('up')
        self.assertEqual(self.shell.focus, 1)
        self.shell.dispatch('back')
        self.assertEqual(self.shell.screen, 'login')

    def test_buttons_dispatch_and_responsive_blocked_launch(self):
        shell = self.shell
        shell.dispatch('activate')
        shell.dispatch('down')
        shell.dispatch('activate')
        wait_action(shell)
        shell.dispatch('right')
        shell.dispatch('activate')
        shell.dispatch('activate')  # no duplicate worker
        shell.dispatch('up')
        shell.dispatch('right')
        self.assertEqual(shell.selected, 1)
        self.runtime.release.set()
        wait_action(shell)
        self.assertEqual(shell.result.exit_code, 2)
        self.assertEqual(shell.result.status, 'blocked')
        self.assertEqual(self.runtime.calls, [('install', 'hello-demo'), ('launch', 'hello-demo')])

    def test_relogin_resets_details_and_both_button_targets(self):
        shell = self.shell
        for action in ('activate', 'right', 'back', 'activate'):
            shell.dispatch(action)
        self.assertEqual(shell.focus, 0)
        self.assertEqual(shell.games[shell.selected].id, 'hello-demo')
        # Rendering labels must not change the selected action target.
        self.assertEqual(shell.focus_label(), 'hello-demo')
        shell.dispatch('down')
        shell.dispatch('activate')
        wait_action(shell)
        shell.dispatch('right')
        shell.dispatch('activate')
        self.runtime.release.set()
        wait_action(shell)
        self.assertEqual(self.runtime.calls,
                         [('install', 'hello-demo'), ('launch', 'hello-demo')])

    def test_non_demo_passes_selected_id_to_adapter(self):
        shell = self.shell
        shell.dispatch('activate')
        shell.dispatch('right')
        shell.dispatch('down')
        shell.dispatch('right')
        shell.dispatch('activate')
        self.runtime.release.set()
        wait_action(shell)
        self.assertEqual(self.runtime.calls, [('launch', 'mock-orbit')])


if __name__ == '__main__':
    unittest.main()
