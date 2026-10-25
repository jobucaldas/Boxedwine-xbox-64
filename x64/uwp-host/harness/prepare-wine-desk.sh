#!/usr/bin/env bash
# Build Wine's explorer sources as an unused private PE DLL (B34 O1).
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
./prepare-wine-wx.sh
out=$PWD/out
tree=$out/wine-wx
source=$out/wine-wx-source
build=$out/wine-wx-build
stamp=$(cat "$tree/.stamp" "$source/programs/explorer/Makefile.in" \
    "$source"/programs/explorer/*.[ch] "$source"/programs/explorer/*.rc \
    prepare-wine-desk.sh | sha256sum | cut -d' ' -f1)
staged=$tree/lib/wine/x86_64-windows/uwpdesktop.dll
if [[ -f $tree/.desk-stamp && $(cat "$tree/.desk-stamp") == "$stamp" && -f $staged ]]; then exit 0; fi
mkdir -p "$source/dlls/uwpdesktop"
# Keep explorer's source and import lists in sync with this exact Wine tree.
python3 -I - "$source" <<'PY'
import pathlib, sys
src = pathlib.Path(sys.argv[1])
text = (src / 'programs/explorer/Makefile.in').read_text()
text = text.replace('MODULE    = explorer.exe', 'MODULE = uwpdesktop.dll\nPARENTSRC = ../../programs/explorer')
text = text.replace('EXTRADLLFLAGS = -mwindows -municode', '')
text = text.rstrip() + ' \\\n\tuwpdesktop.spec \\\n\tuwpdesktop_entry.c\n'
(src / 'dlls/uwpdesktop/Makefile.in').write_text(text)
PY
cat > "$source/dlls/uwpdesktop/uwpdesktop.spec" <<'SPEC'
@ stdcall uwp_desktop_start(long)
SPEC
cat > "$source/dlls/uwpdesktop/uwpdesktop_entry.c" <<'ENTRY'
#include <stdarg.h>
#define WIN32_NO_STATUS
#include "windef.h"
#include "winternl.h"
#undef WIN32_NO_STATUS
#include "ntstatus.h"

NTSTATUS WINAPI uwp_desktop_start(DWORD timeout_ms)
{
    (void)timeout_ms;
    return STATUS_NOT_IMPLEMENTED;
}
ENTRY
# Extend the existing restricted configure, leaving explorer.exe untouched.
python3 -I - "$build/config.status" "$source" <<'PY'
import pathlib, re, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
modules = 'rpcrt4 user32 gdi32 advapi32 win32u comctl32 shell32 oleaut32 ole32 shlwapi kernel32'.split()
source = pathlib.Path(sys.argv[2])
match = re.search(r'(?ms)^SUBDIRS = (.*?)(?=^DISABLED_SUBDIRS =)', text)
assert match, 'expected Wine SUBDIRS in config.status'
subdirs = match[1].split()
# Import libraries need only Wine's specs; avoid scanning resources omitted
# by Debian's repack in modules that we are not rebuilding.
subdirs = [d for d in subdirs if d not in ['dlls/' + m for m in modules]]
if 'dlls/uwpdesktop' not in subdirs:
    subdirs.append('dlls/uwpdesktop')
for module in modules:
    name = 'dlls/uwpdesktop-import-' + module
    directory = source / name
    directory.mkdir(exist_ok=True)
    (directory / 'Makefile.in').write_text(f'MODULE = {module}.dll\nIMPORTLIB = {module}\n')
    spec = directory / (module + '.spec')
    if not spec.exists():
        spec.symlink_to('../' + module + '/' + module + '.spec')
    if name not in subdirs:
        subdirs.append(name)
text = text[:match.start()] + 'SUBDIRS = ' + ' '.join(subdirs) + '\n' + text[match.end():]
path.write_text(text)
PY
cd "$build"
./config.status > desk-configure.log 2>&1 || { tail -40 desk-configure.log >&2; exit 1; }
make -j"$(nproc)" dlls/uwpdesktop/x86_64-windows/uwpdesktop.dll > desk-build.log 2>&1 || { tail -40 desk-build.log >&2; exit 1; }
mkdir -p "$(dirname "$staged")"
cp dlls/uwpdesktop/x86_64-windows/uwpdesktop.dll "$staged"
# The loader's Debian-layout DLL directory also exposes the private module.
cp dlls/uwpdesktop/x86_64-windows/uwpdesktop.dll "$tree/lib/x86_64-linux-gnu/wine/x86_64-windows/uwpdesktop.dll"
printf '%s\n' "$stamp" > "$tree/.desk-stamp"
echo "prepare-wine-desk: built $staged"
