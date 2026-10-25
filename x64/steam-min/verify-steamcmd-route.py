#!/usr/bin/env python3
"""Verify only the selected THROUGH surface for both steamcmd session probes.

Linux harness only: external stock wineserver, nofork OFF; no console claim.
"""
import pathlib
import re
import sys


def verify(evidence, reports, harness):
    selected = {'open', 'read', 'close', 'mmap', 'munmap', 'mprotect'}
    ntdll = (harness / 'out/wine-wx/lib/x86_64-linux-gnu/wine/x86_64-unix/ntdll.so').resolve(strict=True)
    for tag in ('probe1', 'probe2'):
        trace = (evidence / f'{tag}.exec').read_text()
        pids = re.findall(r'^(\d+)\s+execve\("' + re.escape(str(harness / 'out/run_via_wine')) + r'",.*= 0$', trace, re.M)
        assert len(pids) == 1, f'{tag}: missing/ambiguous launcher PID'
        pid = pids[0]
        log = (evidence / f'{tag}-wine.log').read_text()
        assert f'[launcher] exec {harness}/out/wine-wx/lib/wine/wine64 -> ' in log
        assert "steamapi_probe.exe (handing guest to Wine's loader)" in log
        assert 'uwpjail: seccomp wx=1 nofork=0' in log
        totals, modules, classes, report_pids = {}, {}, {}, []
        for line in (reports / f'{tag}.report.{pid}').read_text().splitlines():
            row = line.split()
            if row[0] == 'pid':
                report_pids.append(row[1])
            elif row[0] == 'total':
                _, op, backend, bypass = row
                assert op not in totals
                totals[op] = (int(backend), int(bypass))
            elif row[0] == 'module':
                _, module, op, calls, backend = row
                key = (pathlib.Path(bytes.fromhex(module).decode()).resolve(), op)
                assert key not in modules
                modules[key] = (int(calls), int(backend))
            elif row[0] == 'classification':
                assert not classes
                classes = dict(item.split('=') for item in row[1:])
            elif row[0] == 'trace':
                assert len(row) == 3 and int(row[2]) >= 0
            else:
                raise AssertionError(f'unknown report row: {line}')
        assert report_pids == [pid], f'{tag}: report PID mismatch'
        assert classes == dict(recursion='0', unattributed='0', module_overflow='0', instrumentation_raw_io='1'), classes
        assert set(totals) == selected
        for op in selected:
            calls, backend = modules.get((ntdll, op), (0, 0))
            assert calls == backend and backend > 0, f'{tag}: missing ntdll {op} backend'
            assert totals[op][1] == 0, f'{tag}: {op} bypass'
            assert all(c == b and c >= 0 for (_, operation), (c, b) in modules.items() if operation == op)
            assert totals[op][0] == sum(b for (_, operation), (_, b) in modules.items() if operation == op)
        print(f'{tag}: selected THROUGH surface verified, guest Linux PID={pid}, ntdll={ntdll}, bypass=0, classification clean')
    print('Linux harness only; stock external wineserver; nofork OFF; no Xbox console PASS.')


if __name__ == '__main__':
    try:
        verify(*(pathlib.Path(arg).resolve() for arg in sys.argv[1:]))
    except (AssertionError, OSError, ValueError, IndexError) as error:
        print(f'route verification FAILED: {error}', file=sys.stderr)
        sys.exit(1)
