#!/usr/bin/env python3
"""O4: require every capped O1 symbol counted and bound from real ntdll.

B914: extra traced names (B06's wider set) are accepted only when listed as
routed in surface.tsv or as uwph-trace in surface-trace.tsv; an unknown extra
still fails.
"""
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parent
trace_rows = [row.split('\t') for row in
              (root / 'surface-trace.tsv').read_text().splitlines()[1:]]
selected = [row[2] for row in trace_rows[:20]]
assert len(selected) == len(set(selected)) == 20
listed = {row[2] for row in trace_rows if row[1] == 'uwph-trace'}
listed |= {row.split('\t')[0] for row in
           (root / 'surface.tsv').read_text().splitlines()[1:]
           if row.split('\t')[1] == 'routed'}
traces = {}
for line in pathlib.Path(sys.argv[1]).read_text().splitlines():
    fields = line.split()
    if fields and fields[0] == 'trace':
        _, name, count = fields
        assert name not in traces, name
        traces[name] = int(count)
        assert traces[name] >= 0, (name, traces[name])
missing = [name for name in selected if name not in traces]
assert not missing, ('selected names missing from report', missing)
extras = sorted(set(traces) - set(selected))
unknown = [name for name in extras if name not in listed]
assert not unknown, ('traced names not in surface tables', unknown)
bindings = pathlib.Path(sys.argv[2]).read_text().splitlines()
for name in selected:
    assert traces[name] > 0, (name, traces[name])
    assert any('binding file ' in line and 'ntdll.so [0] to ' in line
               and 'libuwph_wine_shim.so [0]' in line
               and f"symbol `{name}'" in line for line in bindings), name
    print(f'PASS traced {name} entry_count={traces[name]} ntdll_binding=shim')
print('PASS capped O1 traced set: 20/20 counted uwph entries and ntdll shim bindings; remaining imports/direct syscalls excluded')
zero = [name for name in extras if not traces[name]]
print(f'PASS extra traced names: {len(extras)} all listed in surface tables; '
      f'{len(extras) - len(zero)} counted, zero-count (listed): {",".join(zero) or "none"}')
