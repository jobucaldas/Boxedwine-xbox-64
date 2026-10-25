#!/usr/bin/env python3
"""Collector regression checks, without starting Steam or changing the trace."""
import runpy
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch


class CollectorTest(unittest.TestCase):
    def test_current_run_and_resumed_exec(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            evidence = root / 'run.current'
            evidence.mkdir()
            stale = root / 'run.old'
            stale.mkdir()
            (stale / 'client.counters.99').write_text('total socket 999\n')
            source = root / 'alternate-prefix-bootstrap.txt'
            source.write_bytes(b'old launch\nnew launch\n')
            (evidence / 'bootstrap.before').write_bytes(b'old launch\n')
            for phase in ('probe', 'client'):
                (evidence / f'{phase}.log').write_text('')
                (evidence / f'{phase}.exit').write_text('124\n')
                (evidence / f'{phase}.strace').write_text(
                    '12 execve("/loader", ["loader"], 0x123 <unfinished ...>\n'
                    '13 execve("/failed", [], 0x123) = -1 ENOENT\n'
                    '12 <... execve resumed>) = 0\n')
            output = []
            original_write = Path.write_text

            def capture(path, data, *args, **kwargs):
                if path.name == 'surface-trace-steam-client.tsv':
                    output.append(data)
                    return len(data)
                return original_write(path, data, *args, **kwargs)

            with patch.object(sys, 'argv', ['summarize.py', str(evidence), str(source)]), \
                    patch.object(Path, 'write_text', capture):
                runpy.run_path(str(Path(__file__).with_name('summarize.py')))
            trace = output[0]
            self.assertIn('new launch', trace)
            self.assertNotIn('old launch', trace)
            self.assertNotIn('999', trace)
            self.assertIn('0\t{}; reports=0', trace)
            self.assertEqual(trace.count('\tlinux-exec\t'), 2)
            self.assertIn('12 execve("/loader", ["loader"], 0x123 ) = 0', trace)
            self.assertNotIn('/failed', trace)
            # A replaced/truncated bootstrap log must fail rather than use old evidence.
            source.write_bytes(b'replaced log\n')
            with patch.object(sys, 'argv', ['summarize.py', str(evidence), str(source)]):
                with self.assertRaises(RuntimeError):
                    runpy.run_path(str(Path(__file__).with_name('summarize.py')))


if __name__ == '__main__':
    unittest.main()
