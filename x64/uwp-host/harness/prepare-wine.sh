#!/usr/bin/env bash
# Installed matching Wine only; no downloads, source edits or prefix initialization.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out/wine
python3 - <<'PY'
import hashlib,json,pathlib,subprocess
paths = dict(loader='/usr/lib/wine/wine64', pe_ntdll='/usr/lib/x86_64-linux-gnu/wine/x86_64-windows/ntdll.dll', unix_ntdll='/usr/lib/x86_64-linux-gnu/wine/x86_64-unix/ntdll.so')
version=subprocess.check_output([paths['loader'],'--version'],text=True).strip()
assert version.startswith('wine-10.0 '), version
source=pathlib.Path('/tmp/wine-src')
assert (source/'VERSION').read_text().strip()=='Wine version 10.0'
print('Wine version 10.0'); print(version)
def identity(p):
 p=pathlib.Path(p).resolve(); return dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest())
data=dict(version=version, selection='installed matching Debian Wine; no source build or host patch', artifacts={k:identity(v) for k,v in paths.items()})
# Preserve a content manifest of the local source, including loader and Unix side.
files=sorted(p for p in source.rglob('*') if p.is_file())
manifest=''.join(identity(p)['sha256']+'  '+str(p.relative_to(source))+'\n' for p in files)
pathlib.Path('out/wine/source.sha256').write_text(manifest)
data['source_manifest_sha256']=hashlib.sha256(manifest.encode()).hexdigest()
data['source']=str(source); data['source_file_count']=len(files)
data['configuration']='Installed distro build; upstream configure argv unavailable. See package identity and ELF dynamic metadata. Local source not compiled; no configure run.'
data['packages']=subprocess.check_output(['dpkg-query','-W','-f=${binary:Package}\t${Version}\t${Architecture}\n','wine64','libwine'],text=True)
pathlib.Path('out/wine/identity.json').write_text(json.dumps(data,indent=2)+'\n')
for k,v in data['artifacts'].items(): print(k+': '+v['path']+' sha256='+v['sha256'])
print('local source files:',len(files),'manifest sha256='+data['source_manifest_sha256'])
PY
./inventory.sh
make --no-print-directory out/libuwph_shim.so
cat > out/wine/bind.c <<'C'
#include <dlfcn.h>
#include <stdio.h>
int main(int argc, char **argv) {
    if (argc != 2) return 2;
    void *p = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!p) { fprintf(stderr, "%s\n", dlerror()); return 1; }
    puts("real ntdll.so relocation diagnostic: PASS (no Wine entry point invoked)");
    return 0;
}
C
${CC:-gcc} -Wall -Wextra -Werror out/wine/bind.c -ldl -o out/wine/bind
# Scoped to our diagnostic process. No PE, Wine startup or wineserver invocation.
ntdll=$(python3 -c 'import json; print(json.load(open("out/wine/identity.json"))["artifacts"]["unix_ntdll"]["path"])')
timeout 15s env LD_DEBUG=bindings LD_PRELOAD="$PWD/out/libuwph_shim.so" out/wine/bind "$ntdll" >out/wine/bind.stdout 2>out/wine/bind.log
cat out/wine/bind.stdout
