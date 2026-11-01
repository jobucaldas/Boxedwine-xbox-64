"""Offline controller-style terminal shell; Linux prototype, Xbox pending."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import curses
import json
from pathlib import Path
import time

from model import OfflineSteamBackend
from runtime import Runtime, run


class Shell:
    """One dispatcher shared by curses keys, headless scenarios and tests."""

    def __init__(self, runtime=None):
        self.backend = OfflineSteamBackend()
        self.runtime = runtime or Runtime()
        self.worker = ThreadPoolExecutor(max_workers=1)
        self.screen = 'login'
        self.games = ()
        self.focus = 0
        self._selected = 0
        self.logs = ['LOGIN PLACEHOLDER — offline mock approval only']
        self.future = None
        self.result = None
        self.quit = False

    def dispatch(self, action):
        if action == 'back':
            if self.screen == 'login':
                self.quit = True
            else:
                self.screen = 'login'
                self.backend.cancel_login()
                self.focus = 0
                self._selected = 0
            return
        if self.screen == 'login':
            if action == 'activate':
                self.backend.begin_login()
                self.backend.approve_mock_login()
                self.games = self.backend.library().games
                self.screen = 'library'
                self.focus = 0
                self._selected = 0
                self.logs.append(f'MOCK LIBRARY: {len(self.games)} games')
            return
        if action in ('left', 'right', 'up', 'down'):
            # Three tiles in one grid row, two action buttons below.
            if action == 'down':
                self.focus = 3 if self.focus < 3 else self.focus
            elif action == 'up':
                self.focus = self.selected if self.focus >= 3 else self.focus
            elif self.focus < 3:
                self.focus = (self.focus + (1 if action == 'right' else -1)) % 3
            else:
                self.focus = 4 if self.focus == 3 else 3
            if self.focus < 3:
                self._selected = self.focus
            self.logs.append(f'FOCUS: {self.focus_label()}')
        elif action == 'activate' and self.focus >= 3:
            if self.future is not None:
                self.logs.append('BUSY: current action still running; navigation remains available')
                return
            game = self.games[self.selected].id
            self.result = None
            if self.focus == 3:
                self.future = self.worker.submit(self.runtime.install, game)
            else:
                self.future = self.runtime.launch_async(game, self.worker)
            self.logs.append(f'WORKING: {self.focus_label()} {game}')

    @property
    def selected(self):
        return self._selected

    def focus_label(self):
        if self.focus < 3:
            return self.games[self.focus].id
        return ('Install', 'Launch')[self.focus - 3]

    def poll(self):
        if self.future is not None and self.future.done():
            try:
                self.result = self.future.result()
                self.logs.extend(self.result.output.splitlines())
            except Exception as error:
                from runtime import Result
                self.result = Result('error', 1, f'UI WORKER ERROR: {error}')
                self.logs.append(self.result.output)
            self.future = None

    def close(self):
        self.worker.shutdown(wait=True)


def wait_action(shell):
    while shell.future is not None:
        shell.poll()
        time.sleep(0.02)


def headless(shell, scenario):
    print(shell.logs[0], flush=True)
    shell.dispatch('activate')
    if scenario == 'navigation':
        for action in ('right', 'right', 'left', 'down', 'right', 'up'):
            shell.dispatch(action)
        print('\n'.join(shell.logs[1:]))
        print('NAVIGATION PASS')
        return 0
    shell.dispatch('down')
    shell.dispatch('activate')
    wait_action(shell)
    print('\n'.join(shell.logs[1:]), flush=True)
    if shell.result.exit_code:
        return shell.result.exit_code
    printed = len(shell.logs)
    shell.dispatch('right')
    shell.dispatch('activate')
    wait_action(shell)
    print('\n'.join(shell.logs[printed:]), flush=True)
    return shell.result.exit_code


class VisibleDemo:
    """Timed inputs through the interactive dispatcher; no replayed results."""

    def __init__(self, evidence):
        self.evidence = Path(evidence)
        self.started = time.monotonic()
        self.next_at = self.started + 3
        self.step = 0
        self.actions = ('activate', 'right', 'right', 'left', 'left',
                        'down', 'activate', 'right', 'activate')
        self.finished = None

    def tick(self, shell):
        now = time.monotonic()
        if shell.future is not None or now < self.next_at:
            return
        if self.step in (7, 9):
            result = shell.result
            name = 'install' if self.step == 7 else 'launch'
            if not (self.evidence / (name + '.exit')).exists():
                (self.evidence / (name + '.exit')).write_text(str(result.exit_code) + '\n')
                (self.evidence / (name + '.log')).write_text(result.output + '\n')
                if name == 'install' and result.exit_code != 0:
                    raise RuntimeError('demo install failed')
        if self.step < len(self.actions):
            action = self.actions[self.step]
            shell.dispatch(action)
            self.step += 1
            self.next_at = now + 2
        elif self.finished is None:
            result = shell.result
            good = result.status == 'blocked' and result.exit_code == 2
            (self.evidence / 'scenario.json').write_text(json.dumps({
                'completed': good, 'actions': list(self.actions),
                'launch_status': result.status, 'launch_exit': result.exit_code,
                'live_child_log': result.log_path,
                'label': 'Linux prototype — Xbox validation pending'}, indent=2) + '\n')
            (self.evidence / 'ui.log').write_text('\n'.join(shell.logs) + '\n')
            if not good:
                raise RuntimeError('expected live LAUNCH BLOCKED / exit 2')
            self.finished = now
        elif now - self.finished >= 5:
            shell.quit = True


def visible(stdscr, shell, demo=None):
    curses.curs_set(0)
    stdscr.timeout(100)
    keys = {curses.KEY_LEFT: 'left', curses.KEY_RIGHT: 'right',
            curses.KEY_UP: 'up', curses.KEY_DOWN: 'down',
            10: 'activate', 13: 'activate', ord('a'): 'activate',
            27: 'back', ord('b'): 'back'}
    while not shell.quit:
        shell.poll()
        if demo:
            demo.tick(shell)
        stdscr.erase()
        height, width = stdscr.getmaxyx()
        rows = ['Linux prototype — Xbox validation pending',
                'Arrows / D-pad style: move | Enter/A: activate | Esc/B: back',
                'Physical gamepad support deferred. No real Steam login.', '']
        if shell.screen == 'login':
            rows += ['LOGIN PLACEHOLDER', '> [ Approve OFFLINE mock login ]',
                     'No QR, credentials, accounts or network.']
        else:
            rows += ['MOCK LIBRARY: 3 games', ' | '.join(
                ('> ' if shell.focus == i else '  ') + '[' + game.id + ']'
                for i, game in enumerate(shell.games)),
                'Details: ' + shell.games[shell.selected].title,
                'Only hello-demo prepares an owned fixture; other games unavailable.',
                ('> ' if shell.focus == 3 else '  ') + '[Install]    ' +
                ('> ' if shell.focus == 4 else '  ') + '[Launch]',
                'Launch: live R4H batch, routed uwph runs + stock controls.',
                'Status: ' + ('WORKING (navigation available)' if shell.future else
                              shell.result.status.upper() if shell.result else 'IDLE')]
        rows += ['', 'LOG (latest lines; full raw launch logs retained in ui/out):']
        available = max(0, height - len(rows) - 1)
        rows += shell.logs[-available:] if available else []
        for y, line in enumerate(rows[:max(0, height - 1)]):
            try:
                stdscr.addnstr(y, 0, line, max(0, width - 1))
            except curses.error:
                pass
        stdscr.refresh()
        key = stdscr.getch()
        if key in keys:
            shell.dispatch(keys[key])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--headless', action='store_true')
    parser.add_argument('--scenario', choices=('navigation', 'launch-hello'), default='navigation')
    parser.add_argument('--demo-evidence', type=Path, help='bounded visible live demo output directory')
    args = parser.parse_args()
    if args.demo_evidence and args.headless:
        parser.error('visible demo requires curses')
    if args.demo_evidence:
        args.demo_evidence.mkdir(parents=True, exist_ok=True)
    shell = Shell(Runtime(runner=lambda command, cwd: run(command, cwd, timeout=20))
                  if args.demo_evidence else None)
    try:
        if args.headless:
            return headless(shell, args.scenario)
        curses.wrapper(visible, shell,
                       VisibleDemo(args.demo_evidence) if args.demo_evidence else None)
        return 0
    finally:
        shell.close()


if __name__ == '__main__':
    raise SystemExit(main())
