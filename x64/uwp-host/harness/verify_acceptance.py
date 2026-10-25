#!/usr/bin/env python3
"""O4: require every capped O1 symbol counted and bound from real ntdll."""
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent
selected = [row.split('\t')[2] for row in
            (root / 'surface-trace.tsv').read_text().splitlines()[1:21]]
assert len(selected) == len(set(selected)) == 20
traces = {}
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    fields = line.split()
    if fields[0] == 'trace':
        _, name, count = fields
        assert name not in traces, name
        traces[name] = int(count)
assert set(traces) == set(selected), traces
bindings = pathlib.Path(sys.argv[2]).read_text().splitlines()
for name in selected:
    assert traces[name] > 0, (name, traces[name])
    assert any('binding file ' in line and 'ntdll.so [0] to ' in line
               and 'libuwph_wine_shim.so [0]' in line
               and f"symbol `{name}'" in line for line in bindings), name
    print(f'PASS traced {name} entry_count={traces[name]} ntdll_binding=shim')
print('PASS capped O1 traced set: 20/20 counted uwph entries and ntdll shim bindings; remaining imports/direct syscalls excluded')
