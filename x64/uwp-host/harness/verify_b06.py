#!/usr/bin/env python3
"""Bounded B06 surface delta from actual launcher/Wine-loader evidence."""
import pathlib
import sys

run = pathlib.Path(sys.argv[1])
root = pathlib.Path(__file__).resolve().parent
rows = [line.split('\t') for line in (root / 'surface-trace.tsv').read_text().splitlines()[1:36]]
selected = [row[2] for row in rows]
assert len(set(selected)) == 35
for pe in ('hello_k32', 'hello'):
    name = 'routed-' + pe
    assert (run / (name + '.exit')).read_text().strip() == '0'
    log = (run / (name + '.log')).read_text()
    assert '[launcher]' in log
    assert ('TEB self-check: ok' if pe == 'hello' else 'hello ') in log
    reports = list(run.glob(name + '.report.*'))
    assert reports
    candidates = []
    for report in reports:
        counts = {f[1]: int(f[2]) for line in report.read_text().splitlines()
                  if (f := line.split()) and f[0] == 'trace'}
        assert set(counts) == set(selected)
        candidates.append(counts)
    # Keep all symbols in a single process; do not combine child reports.
    counts = max(candidates, key=lambda c: sum(c[s] > 0 for s in selected))
    for symbol in selected:
        assert any('binding file ' in line and 'ntdll.so [0] to ' in line
                   and 'libuwph_wine_shim.so [0]' in line
                   and f"symbol `{symbol}'" in line for line in log.splitlines()), symbol
    assert all(counts[s] > 0 for s in selected[:20])
    delta = sum(counts[s] > 0 for s in selected[20:])
    assert delta >= 10, delta
    output = ['symbol\tstatus\tentry_count']
    output += [f'{s}\t{"THROUGH-uwph" if counts[s] else "bound-unhit"}\t{counts[s]}' for s in selected]
    (run / (name + '.surface.tsv')).write_text('\n'.join(output) + '\n')
    print(f'{name}: surface THROUGH-uwph 20 -> {20 + delta} (+{delta}); '
          f'bound-unhit: {",".join(s for s in selected if not counts[s])}')
print('PASS B06 surface delta; Linux off-device, launcher -> Wine own loader -> shim -> uwph; separate stock wineserver')
