#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")"
python3 - <<'PY'
import json,pathlib,re,subprocess
out=pathlib.Path('out/wine'); data=json.loads((out/'identity.json').read_text())
p=data['artifacts']['unix_ntdll']['path']
# Retain raw symbol versions, binding strength, relocations, and dynamic dependencies.
for name,args in [('symbols',['readelf','--dyn-syms','--wide',p]),('relocations',['readelf','-rW',p]),('dynamic',['readelf','-dW',p]),('disassembly',['objdump','-d',p])]:
 (out/(name+'.txt')).write_text(subprocess.check_output(args,text=True))
imports=[]
for line in (out/'symbols.txt').read_text().splitlines():
 fields=line.split()
 if len(fields)>=8 and fields[6]=='UND':
  sym=fields[7]
  if sym: imports.append((sym.split('@')[0], sym.partition('@')[2] or 'Base',fields[4]))
assert imports
(out/'imports.tsv').write_text('symbol\tversion\tbinding\n'+''.join('\t'.join(row)+'\n' for row in sorted(imports)))
# Actual decoded instruction sites, not a byte search. All are gaps irrespective
# of whether they implement a host syscall or a Wine syscall dispatch path.
sites=[' '.join(line.split()) for line in (out/'disassembly.txt').read_text().splitlines() if re.search(r'\bsyscall\s*$',line)]
assert sites, 'expected x86-64 direct syscall instructions'
(out/'direct-syscalls.tsv').write_text('instruction_site\tclassification\n'+''.join(s+'\tunsupported-direct-site\n' for s in sites))
print(f'inventory: {len(imports)} imports; {len(sites)} direct syscall instruction sites')
PY
