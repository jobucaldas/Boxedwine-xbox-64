#!/usr/bin/env bash
# Installed matching Wine; prefix initialization is packaging-only and opt-in.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p out/wine
if [[ ${1:-} == --prebuilt-prefix ]]; then
    # Separate build-time path: never called by the runtime or wine-test.
    exec 9>out/wine/prebuilt-prefix.lock
    flock -n 9 || { echo 'Prefix packaging already running' >&2; exit 1; }
    python3 - <<'PACK'
import hashlib, json, os, pathlib, subprocess, tempfile

loader = pathlib.Path('/usr/lib/wine/wine64')
server = pathlib.Path('/usr/lib/wine/wineserver64')
inf = pathlib.Path('/usr/share/wine/wine.inf')
def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()
version = subprocess.check_output([str(loader), '--version'], text=True).strip()
if not version.startswith('wine-10.0 '):
    raise SystemExit('Expected matching installed Wine 10.0: ' + version)
identity = dict(version=version, loader_sha256=digest(loader),
                wineserver_sha256=digest(server), wine_inf_sha256=digest(inf),
                wine_inf_mtime=int(inf.stat().st_mtime),
                purpose='Build-time stock Wine prefix; not THROUGH-uwph runtime proof')
dest = pathlib.Path('prebuilt-prefix').absolute()
def validate(prefix):
    if json.loads((prefix / 'packaging.json').read_text()) != identity:
        raise SystemExit('Packaged Wine identity changed; remove the generated prefix and rebuild')
    if (prefix / '.update-timestamp').read_text() != str(identity['wine_inf_mtime']) + '\n':
        raise SystemExit('Wine-generated .update-timestamp does not match wine.inf')
    for name in ('system.reg', 'user.reg', 'userdef.reg'):
        if not (prefix / name).is_file() or not (prefix / name).stat().st_size:
            raise SystemExit('Missing registry: ' + name)
    for entry in [prefix, *prefix.rglob('*')]:
        if not entry.is_symlink() and entry.stat().st_mode & 0o222:
            raise SystemExit('Packaged prefix is writable: ' + str(entry))
if dest.exists():
    validate(dest)
    print('Reused read-only prebuilt-prefix; wineboot invocations this run: 0')
else:
    stage = pathlib.Path(tempfile.mkdtemp(prefix='prefix-build-', dir='out/wine')).absolute()
    prefix = stage / 'prefix'
    env = os.environ.copy()
    for key in ('LD_PRELOAD', 'LD_DEBUG', 'WINEDLLOVERRIDES', 'WINELOADER',
                'WINE_LOADER', 'WINESERVER', 'WINEPREFIX', 'WINEARCH'):
        env.pop(key, None)
    env.update(WINEPREFIX=str(prefix), WINEARCH='win64', WINESERVER=str(server),
               WINEDEBUG='-all')
    try:
        with (stage / 'wineboot.log').open('w') as log:
            print('Packaging only: invoking installed Wine wineboot.exe --init once', flush=True)
            subprocess.run([str(loader), 'wineboot.exe', '--init'], env=env,
                           stdout=log, stderr=subprocess.STDOUT, timeout=120, check=True)
            subprocess.run([str(server), '-w'], env=env, stdout=log,
                           stderr=subprocess.STDOUT, timeout=60, check=True)
        (prefix / 'packaging.json').write_text(json.dumps(identity, indent=2) + '\n')
        # Do not follow Wine drive links into the host when setting permissions.
        for base, dirs, files in os.walk(prefix, followlinks=False):
            for name in files:
                path = pathlib.Path(base) / name
                if not path.is_symlink():
                    path.chmod(path.stat().st_mode & ~0o222)
            pathlib.Path(base).chmod(pathlib.Path(base).stat().st_mode & ~0o222)
        validate(prefix)
        # Moving a directory changes its parent entry; temporarily allow that.
        mode = prefix.stat().st_mode
        prefix.chmod(mode | 0o200)
        try:
            prefix.rename(dest)
        finally:
            (dest if dest.exists() else prefix).chmod(mode)
        validate(dest)
        print('Created read-only prebuilt-prefix; stamp=' + str(identity['wine_inf_mtime']))
        print('Build log: ' + str(stage / 'wineboot.log'))
    except BaseException:
        subprocess.run([str(server), '-k'], env=env, timeout=10, check=False)
        print('Packaging failed; diagnostic directory: ' + str(stage))
        raise
print('Build-only stock Wine preparation; no runtime acceptance claimed')
PACK
    exit 0
elif [[ $# != 0 ]]; then
    echo "Usage: $0 [--prebuilt-prefix]" >&2
    exit 2
fi
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
