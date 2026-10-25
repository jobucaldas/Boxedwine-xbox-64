#!/usr/bin/env python3
"""Require fresh shim evidence of ntdll routing for the noproc hello run."""
import pathlib
import sys

totals, modules, classes = {}, {}, {}
pid = None
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    fields = line.split()
    if fields[0] == 'pid':
        assert pid is None
        pid = int(fields[1])
    elif fields[0] == 'total':
        _, op, backend, bypass = fields
        assert op not in totals
        totals[op] = (int(backend), int(bypass))
    elif fields[0] == 'module':
        _, name, op, calls, backend = fields
        key = (bytes.fromhex(name).decode(), op)
        assert key not in modules
        modules[key] = (int(calls), int(backend))
    elif fields[0] == 'classification':
        assert not classes
        classes = dict(item.split('=') for item in fields[1:])
    elif fields[0] != 'trace':
        raise AssertionError(line)
assert pid and pid > 0
assert classes == dict(recursion='0', unattributed='0', module_overflow='0',
                       instrumentation_raw_io='1'), classes
assert set(totals) == {'open', 'read', 'close', 'mmap', 'munmap'}, totals
for op, (backend, bypass) in totals.items():
    assert backend > 0 and bypass == 0, (op, backend, bypass)
    entries = [(name, counts) for (name, operation), counts in modules.items()
               if operation == op]
    assert entries and sum(counts[1] for _, counts in entries) == backend
    assert all(calls == routed and calls >= 0 for _, (calls, routed) in entries)
    assert any(pathlib.Path(name).name == 'ntdll.so' and counts[1] > 0
               for name, counts in entries), (op, entries)
print(f'PASS fresh noproc routing report pid={pid}; ntdll open/read/close/mmap/munmap positive; bypass/recursion/unattributed/module_overflow=0')
