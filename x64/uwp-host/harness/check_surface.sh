#!/usr/bin/env bash
# Classification completeness, explicitly NOT a full portability gate.
set -euo pipefail
cd "$(dirname "$0")"
python3 - "${1:-surface.tsv}" <<'PY'
import collections,csv,hashlib,json,pathlib,re,subprocess,sys
out=pathlib.Path('out/wine'); data=json.loads((out/'identity.json').read_text())
for item in data['artifacts'].values():
 assert hashlib.sha256(pathlib.Path(item['path']).read_bytes()).hexdigest()==item['sha256'], 'artifact identity changed; rerun prepare-wine.sh'
rows=list(csv.DictReader(open(sys.argv[1]),delimiter='\t'))
policy={r['symbol']:r for r in rows}
assert len(rows)==len(policy), 'duplicate policy symbols'
imports=list(csv.DictReader(open(out/'imports.tsv'),delimiter='\t'))
assert set(policy)=={r['symbol'] for r in imports}, 'unclassified or stale imports'
allowed={'routed','libc internal','explicit host fallback','unsupported'}
assert all(r['classification'] in allowed and r['reason'] for r in rows)
log=(out/'bind.log').read_text(); ntdll=data['artifacts']['unix_ntdll']['path']
exports=subprocess.check_output(['nm','-D','--defined-only','out/libuwph_shim.so'],text=True)
export_names={line.split()[-1].split('@')[0] for line in exports.splitlines()}
# O2 traced set plus proc provider/registry wrappers: exact export inventory.
script=pathlib.Path('shim.map').read_text()
selected={x.strip() for x in script.split('global:')[1].split('local:')[0].split(';') if x.strip()}-{'uwph_shim_report'}
assert {'open','read','close','mmap','mprotect','munmap','_exit'}<=selected and len(selected)==32, sorted(selected)
proc_exports={'opendir','readdir','closedir','pthread_create'}
assert proc_exports <= selected
assert all(policy[s]['classification']=='explicit host fallback' for s in proc_exports)
traced=set(re.findall(r'"([^"]+)"',pathlib.Path('../src/trace.c').read_text().split('names[]')[1].split(';')[0]))
assert traced==selected-proc_exports-{'open','read','close','mmap','munmap','mprotect','_exit','readlink'}, 'uwph trace counters != shim traced exports'
assert {s for s,r in policy.items() if r['classification']=='routed'}==selected-proc_exports-{'_exit','readlink'}
# readlink is a Wine loader export, absent from this ntdll import inventory.
assert 'readlink' in export_names
for symbol in sorted(selected-{'readlink'}):
 assert symbol in export_names, 'missing shim export '+symbol
 matches=[l for l in log.splitlines() if f'binding file {ntdll} [0] to ' in l and f"symbol `{symbol}'" in l]
 assert len(matches)==1 and str(pathlib.Path('out/libuwph_shim.so').resolve())+' [0]' in matches[0], 'wrong real relocation binding '+symbol
 print('binding verified: ntdll.so -> uwph shim: '+symbol)
counts=collections.Counter(r['classification'] for r in rows)
print('surface classification: PASS ('+', '.join(f'{k}={v}' for k,v in sorted(counts.items()))+')')
print('full Unix-side coverage: INCOMPLETE; unsupported imports/direct syscall sites block full-coverage claims')
print('LIMITS: routed means selected binding only; read/close/mmap native-fd bypasses remain; _exit is instrumentation/raw Linux fallback')
print('LIMITS: versioned imports verified for this artifact only; hidden/local libc calls, libc internals, dlopen additions and direct syscall instructions bypass interposition')
print('O3 is static/host-only + relocation diagnostic; no PE/launcher execution proof THROUGH uwph (O4 gate pending)')
PY
