#!/usr/bin/env python3
"""Fail closed on Wine identity, attribution, selected bypasses or boot activity.

Routing diagnostics can pass while the overall wineboot-free gate is BLOCKED.
"""
import hashlib
import json
import pathlib
import re
import sys


def verify(run):
    identity = json.loads((run / 'identity.json').read_text())
    for item in identity['artifacts'].values():
        assert hashlib.sha256(pathlib.Path(item['path']).read_bytes()).hexdigest() == item['sha256'], 'artifact changed'
    ntdll = pathlib.Path(identity['artifacts']['unix_ntdll']['path']).resolve()
    shim = (pathlib.Path(__file__).resolve().parent / 'out/libuwph_wine_shim.so').resolve()
    selected = ('open', 'read', 'close', 'mmap', 'munmap', 'mprotect')
    blockers = []
    for mode in ('control', 'routed'):
        for pe in ('hello_k32', 'hello'):
            name = f'{mode}-{pe}'
            log = (run / f'{name}.log').read_text()
            trace = (run / f'{name}.exec').read_text()
            assert (run / f'{name}.exit').read_text().strip() == '0', f'{name}: nonzero exit'
            assert '[launcher] exec ' in log and '(handing guest to Wine\'s loader)' in log, 'missing handoff'
            assert re.search(r'loaddll:build_module Loaded .*' + re.escape(pe + '.exe'), log), 'missing Wine module-load trace'
            if pe == 'hello_k32':
                assert 'hello from a native x86-64 PE (kernel32 only)' in log
            else:
                for expected in ('hello.exe: native x86-64 Windows PE', 'TEB self-check: ok, sizeof(void*)=8',
                                 'malloc: heap', 'threads: shared=200000 exit codes=6,7 main tls=5'):
                    assert expected in log, f'{name}: missing {expected}'
            # Includes failed startup attempts; disabling wineboot cannot green it.
            if 'wineboot' in log.lower() or 'wineboot' in trace.lower():
                blockers.append(f'{name}: wineboot activity/attempt in trace')
            main_pids = re.findall(r'^(\d+)\s+execve\(".*/out/run_via_wine"', trace, re.M)
            assert len(main_pids) == 1, 'ambiguous launcher process'
            report = run / f'{name}.report.{main_pids[0]}'
            if mode == 'control':
                assert not list(run.glob(f'{name}.report*')), 'stock control produced uwph report'
                print(f'{name}: stock Wine / our launcher only; no uwph report')
                continue
            lines = report.read_text().splitlines()
            totals, modules, classes, pids = {}, {}, {}, []
            for line in lines:
                fields = line.split()
                if fields[0] == 'pid':
                    pids.append(int(fields[1]))
                elif fields[0] == 'total':
                    _, op, backend, bypass = fields
                    assert op not in totals
                    totals[op] = (int(backend), int(bypass))
                elif fields[0] == 'module':
                    _, module, op, calls, backend = fields
                    key = (str(pathlib.Path(bytes.fromhex(module).decode()).resolve()), op)
                    assert key not in modules
                    modules[key] = (int(calls), int(backend))
                elif fields[0] == 'classification':
                    assert not classes
                    classes = dict(part.split('=') for part in fields[1:])
                else:
                    raise AssertionError(f'unknown report row: {line}')
            assert len(pids) == 1 and pids[0] > 0, 'multiple/absent process reports'
            assert re.search(rf'^{pids[0]}\s+execve\(".*/out/run_via_wine"', trace, re.M), 'report PID is not launcher/Wine process'
            assert classes == dict(recursion='0', unattributed='0', module_overflow='0', instrumentation_raw_io='1'), classes
            assert set(totals) == set(selected)
            for op in selected:
                assert totals[op][1] == 0, f'unexpected {op} bypass'
                values = modules.get((str(ntdll), op), (0, 0))
                assert values[0] == values[1], f'{op}: wrapper/backend mismatch'
                # Require all six selected families to execute in real ntdll.
                assert values[1] > 0, f'zero ntdll {op} backend entries'
                assert totals[op][0] == sum(v[1] for (_, operation), v in modules.items() if operation == op)
                assert all(c == b for (module, operation), (c, b) in modules.items() if operation == op)
                bindings = [line for line in log.splitlines() if 'binding file ' in line and f"symbol `{op}'" in line
                            and any(str(pathlib.Path(m).resolve()) == str(ntdll)
                                    for m in re.findall(r'binding file (\S+) \[0\]', line))]
                assert bindings and all(f'to {shim} [0]' in line for line in bindings), f'{op}: wrong ntdll binding'
            print(f'{name}: ntdll={ntdll} pid={pids[0]} backend=' + ','.join(
                f'{op}:{modules[(str(ntdll), op)][1]}' for op in selected) + ' bypass=0 (selected surface only)')
    print('Remaining bypasses: 160 unsupported imports, 33 retained libc/internal imports, libc syscall(), two inline syscall sites; see surface.tsv/direct-syscalls.tsv.')
    print('Separate stock Linux wineserver: NOT routed; NOT in-process Xbox/UWP evidence.')
    if blockers:
        raise AssertionError('; '.join(blockers))
    print('Wine uwph verification: PASS')


if __name__ == '__main__':
    try:
        verify(pathlib.Path(sys.argv[1]))
    except (AssertionError, OSError, ValueError, KeyError) as error:
        print(f'Wine uwph verification: BLOCKED: {error}', file=sys.stderr)
        sys.exit(1)
