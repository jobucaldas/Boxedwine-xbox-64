#!/usr/bin/env python3
"""Generate only validated indices, never Valve function RVAs or real denied pointers."""
import hashlib
import importlib.util
import json
from pathlib import Path
import sys
from denylist import denied

spec = importlib.util.spec_from_file_location('validator', Path(__file__).with_name('validate-map.py'))
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)
def generate(mapping, raw):
    lines = ['/* Local generated map: do not publish. */',
             '#define HOST_MAP_SHA256 ' + json.dumps(hashlib.sha256(raw).hexdigest()),
             '#define HOST_MAP_BUILD 1788652215u',
             'static const struct host_slot host_slots[] = {']
    for interface, table in mapping['interfaces'].items():
        for slot in table['slots']:
            # Denied entries have no usable index at all.
            blocked = denied(slot)
            lines.append('{%s,%s,%du,%d,%s},' % (json.dumps(interface), json.dumps(slot['name']),
                                            0 if blocked else v.number(slot['index']), blocked,
                                            "(void *)denied_trap" if blocked else "NULL"))
    lines.append('};')
    return '\n'.join(lines) + '\n'


if __name__ == '__main__':
    source, dll, target = map(Path, sys.argv[1:])
    raw = source.read_bytes()
    mapping = json.loads(raw)
    v.validate(mapping, dll.read_bytes())
    target.parent.mkdir(parents=True, exist_ok=True)
    target.write_text(generate(mapping, raw))
