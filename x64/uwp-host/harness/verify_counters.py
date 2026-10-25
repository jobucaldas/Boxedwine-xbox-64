#!/usr/bin/env python3
"""Verify deterministic operations, independent backend entries and caller identities."""
import pathlib
import sys

expected = {"open": 202, "read": 200, "close": 201, "mmap": 401, "munmap": 401}
bypasses = {"open": 0, "read": 2, "close": 2, "mmap": 0, "munmap": 0}
idle = {"mprotect": (0, 0)}  # routed, but the probe never changes protection
assert not pathlib.Path(sys.argv[1]).exists(), "unshimmed control created a report"
for filename in sys.argv[2:]:
    totals, modules, classes = {}, {}, {}
    pid = None
    for line in pathlib.Path(filename).read_text().splitlines():
        fields = line.split()
        if fields[0] == "pid":
            assert pid is None
            pid = int(fields[1])
        elif fields[0] == "total":
            _, op, backend, bypass = fields
            assert op not in totals
            totals[op] = (int(backend), int(bypass))
        elif fields[0] == "module":
            _, hexname, op, calls, backend = fields
            name = bytes.fromhex(hexname).decode()
            key = (name, op)
            assert key not in modules
            modules[key] = (int(calls), int(backend))
        elif fields[0] == "classification":
            classes = dict(item.split("=") for item in fields[1:])
        else:
            raise AssertionError(line)
    assert pid and pid > 0
    assert totals == {**{op: (n, bypasses[op]) for op, n in expected.items()}, **idle}, totals
    assert classes == dict(recursion="0", unattributed="0", module_overflow="0",
                           instrumentation_raw_io="1"), classes
    for op, n in expected.items():
        callers = [(name, values) for (name, operation), values in modules.items()
                   if operation == op and values != (0, 0)]
        assert len(callers) == 1, callers
        name, values = callers[0]
        assert pathlib.Path(name).name == "elf_probe", callers
        assert values == (n + bypasses[op], n), (op, values)
    print(f"pid={pid} module=elf_probe backend=" +
          ",".join(f"{op}:{n}" for op, n in expected.items()) +
          " bypass=read:2,close:2 recursion=0")
print("counter verification: PASS")
