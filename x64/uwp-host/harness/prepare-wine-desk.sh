#!/usr/bin/env bash
# Build Wine's explorer desktop as a private in-process PE DLL, plus the
# user32/win32u pair carrying the opt-in in-process desktop callback (B35).
set -euo pipefail
cd "$(dirname "$(realpath "$0")")"
./prepare-wine-wx.sh
out=$PWD/out
tree=$out/wine-wx
source=$out/wine-wx-source
build=$out/wine-wx-build
src=${WINE_SRC:-/tmp/wine-src}
lib=$tree/lib/x86_64-linux-gnu/wine
installed=/usr/lib/x86_64-linux-gnu/wine
# Restore pristine sources before applying our reproducible patches.
patched=(programs/explorer/desktop.c include/ntuser.h dlls/win32u/ntuser_private.h
    dlls/win32u/winstation.c dlls/user32/user_main.c dlls/wow64win/user.c)
for f in "${patched[@]}"; do cp "$src/$f" "$source/$f"; done
patch -d "$source" -p1 --forward --no-backup-if-mismatch < wine-desktop-thread.patch
patch -d "$source" -p1 --forward --no-backup-if-mismatch < wine-inproc-desktop.patch
stamp=$(cat "$tree/.stamp" "$source/programs/explorer/Makefile.in" \
    "$source"/programs/explorer/*.[ch] "$source"/programs/explorer/*.rc \
    "${patched[@]/#/$source/}" "$installed/x86_64-windows/user32.dll" \
    prepare-wine-desk.sh wine-desktop-thread.patch wine-inproc-desktop.patch | sha256sum | cut -d' ' -f1)
staged=$tree/lib/wine/x86_64-windows/uwpdesktop.dll
if [[ -f $tree/.desk-stamp && $(cat "$tree/.desk-stamp") == "$stamp" && -f $staged ]]; then exit 0; fi

# Debian's win32u.so dlopens FreeType and fontconfig; their headers come from
# the Debian -dev packages matching the installed runtime libraries. The
# repack also drops generated win32u/vertical.c; Debian regenerates it from
# unicode-data 15 (checked against the installed win32u.so below).
debs=${WINE_DEV_DEBS:-/tmp/R4W-devdebs}
hdr=$out/wine-devhdr
fetch() {
    local deb=$debs/${1}_${2//:/%3a}_${3}.deb
    if [[ ! -f $deb ]]; then
        mkdir -p "$debs"
        (cd "$debs" && apt-get download "$1=$2" > /dev/null)
    fi
    dpkg-deb -x "$deb" "$hdr"
}
fetch libfreetype-dev "$(dpkg-query -W -f '${Version}' libfreetype6:amd64)" amd64
fetch libfontconfig-dev "$(dpkg-query -W -f '${Version}' libfontconfig1:amd64)" amd64
fetch unicode-data 15.1.0-1 all
[[ -f $hdr/usr/include/freetype2/ft2build.h && -f $hdr/usr/include/fontconfig/fontconfig.h ]]
# tools/make_unicode dump_vertical(), ported for this one generated file.
python3 -I - "$hdr/usr/share/unicode/VerticalOrientation.txt" "$source/dlls/win32u/vertical.c" \
    "$installed/x86_64-unix/win32u.so" <<'PY'
import re, struct, sys
types = {'R': 0, 'U': 1, 'Tr': 2, 'Tu': 3}
table = [None] * 65536
for line in open(sys.argv[1], encoding='utf-8'):
    if line.startswith('#') or not line.strip() or '\x1a' in line: continue
    m = re.match(r'\s*([0-9a-fA-F]+)\s*;\s*([a-zA-Z_]+)\s*', line)
    if m:
        if int(m[1], 16) < 65536: table[int(m[1], 16)] = types[m[2]]
        continue
    m = re.match(r'\s*([0-9a-fA-F]+)\.\.\s*([0-9a-fA-F]+)\s*;\s*([A-Za-z_]+)\s*', line)
    assert m, 'malformed line ' + line
    for i in range(int(m[1], 16), min(int(m[2], 16) + 1, 65536)): table[i] = types[m[3]]
def compress(rows, default, values):
    size = len(values) // rows
    array, data = [], ''
    for row in range(rows):
        text = ''.join(chr(default if v is None else v) for v in values[row * size:(row + 1) * size])
        pos = data.find(text)
        if pos == -1:
            i = len(data) - 1
            while i > 0:
                pos = data[-i:].find(text[0])
                if pos == -1: break
                i -= pos
                if data[-i:] == text[:i]:
                    data = data[:-i]
                    break
                i -= 1
            pos = len(data)
            data += text
        array.append(rows + pos)
    return array, [ord(c) for c in data]
rows, values = compress(4096, types['R'], table)
level1, row_data = compress(256, 0, rows)
row_data = [v + len(row_data) + 256 - 4096 for v in row_data]
full = level1 + row_data + values
assert struct.pack('<%dH' % len(full), *full) in open(sys.argv[3], 'rb').read(), \
    'vertical table differs from the installed win32u.so'
def dump(values):
    return '    ' + ''.join('0x%04x' % v + ('' if i == len(values) - 1 else ', ' if i % 8 != 7 else ',\n    ')
                            for i, v in enumerate(values))
with open(sys.argv[2], 'w') as out:
    out.write('/* Unicode Vertical Orientation */\n/* generated from /usr/share/unicode:VerticalOrientation.txt */\n'
              '/* DO NOT EDIT!! */\n\n#if 0\n#pragma makedep unix\n#endif\n\n#include "windef.h"\n\n')
    out.write('const unsigned short vertical_orientation_table[%d] =\n{\n' % len(full))
    out.write('    /* level 1 offsets */\n%s,\n    /* level 2 offsets */\n%s,\n    /* values */\n%s\n};\n'
              % (dump(level1), dump(row_data), dump(values)))
PY
# Likewise win32u/win32syscalls.h: only the win32u step of tools/make_specfiles.
sed '/^foreach my \$group (@dll_groups)/,$d' "$source/tools/make_specfiles" > "$out/make_win32syscalls"
printf 'update_syscalls( "win32u", "dlls/win32u/win32syscalls.h" );\n' >> "$out/make_win32syscalls"
(cd "$source" && perl "$out/make_win32syscalls")

mkdir -p "$source/dlls/uwpdesktop"
# Keep explorer's source and import lists in sync with this exact Wine tree.
python3 -I - "$source" "$src" <<'PY'
import pathlib, sys
src = pathlib.Path(sys.argv[1])
text = (src / 'programs/explorer/Makefile.in').read_text()
text = text.replace('MODULE    = explorer.exe', 'MODULE = uwpdesktop.dll\nPARENTSRC = ../../programs/explorer')
text = text.replace('EXTRADLLFLAGS = -mwindows -municode', '')
text = text.rstrip() + ' \\\n\tuwpdesktop.spec \\\n\tuwpdesktop_entry.c\n'
(src / 'dlls/uwpdesktop/Makefile.in').write_text(text)
# The repack omits generated cursors/icons; user32 links the installed
# user32.dll's resources instead of regenerating them from SVG.
text = (pathlib.Path(sys.argv[2]) / 'dlls/user32/Makefile.in').read_text()
lines = [l for l in text.splitlines() if not l.strip().startswith('resources/') and l.strip() != 'user32.rc \\']
text = '\n'.join(lines).rstrip() + ' \\\n\tuser32-installed.res\n'
(src / 'dlls/user32/Makefile.in').write_text(text)
PY
cat > "$source/dlls/uwpdesktop/uwpdesktop.spec" <<'SPEC'
@ stdcall uwp_desktop_start(long)
SPEC
# Export implementation lives in the shared explorer desktop.c.
printf '/* Entry is implemented in desktop.c. */\n' > "$source/dlls/uwpdesktop/uwpdesktop_entry.c"
mkdir -p "$build/dlls/user32"
# Dump the installed user32.dll resource tree as a .res file (all entries kept);
# makedep finds it in the source dir, the link step in the build dir.
python3 -I - "$installed/x86_64-windows/user32.dll" "$source/dlls/user32/user32-installed.res" <<'PY'
import struct, sys
pe = open(sys.argv[1], 'rb').read()
off = struct.unpack_from('<I', pe, 0x3c)[0]
nsect, optsize = struct.unpack_from('<H12xH', pe, off + 6)
opt = off + 24
assert struct.unpack_from('<H', pe, opt)[0] == 0x20b
rsrc_rva, rsrc_size = struct.unpack_from('<II', pe, opt + 112 + 2 * 8)
sections = [struct.unpack_from('<8xIIII', pe, opt + optsize + 40 * i) for i in range(nsect)]
def fo(rva):
    for vsize, va, rawsize, raw in sections:
        if va <= rva < va + max(vsize, rawsize): return raw + rva - va
    raise ValueError(hex(rva))
base = fo(rsrc_rva)
def name(entry):
    if entry & 0x80000000:
        p = base + (entry & 0x7fffffff)
        n = struct.unpack_from('<H', pe, p)[0]
        return pe[p + 2:p + 2 + 2 * n] + b'\0\0'
    return struct.pack('<HH', 0xffff, entry)
def entries(p):
    named, ids = struct.unpack_from('<12xHH', pe, base + p)
    for i in range(named + ids):
        yield struct.unpack_from('<II', pe, base + p + 16 + 8 * i)
res = bytearray(struct.pack('<IIHHHHIHHII', 0, 32, 0xffff, 0, 0xffff, 0, 0, 0, 0, 0, 0))
count = 0
for tname, tp in entries(0):
    for nname, np_ in entries(tp & 0x7fffffff):
        for lang, dp in entries(np_ & 0x7fffffff):
            rva, size = struct.unpack_from('<II', pe, base + dp)
            head = name(tname) + name(nname)
            head += b'\0' * (-len(head) % 4)
            head += struct.pack('<IHHII', 0, 0x1030, lang, 0, 0)
            res += struct.pack('<II', size, 8 + len(head)) + head
            res += pe[fo(rva):fo(rva) + size] + b'\0' * (-size % 4)
            count += 1
open(sys.argv[2], 'wb').write(res)
print(f'user32 resources: {count}', file=sys.stderr)
PY
cp "$source/dlls/user32/user32-installed.res" "$build/dlls/user32/user32-installed.res"
# Extend the existing restricted configure, leaving explorer.exe untouched.
python3 -I - "$build/config.status" "$source" "$hdr/usr/include" <<'PY'
import pathlib, re, sys
path = pathlib.Path(sys.argv[1])
text = path.read_text()
# Import-only stubs; user32 and win32u are real modules below.
modules = ('rpcrt4 gdi32 advapi32 comctl32 shell32 oleaut32 ole32 shlwapi kernel32 '
           'sechost kernelbase imm32 combase').split()
source = pathlib.Path(sys.argv[2])
include = sys.argv[3]
match = re.search(r'(?ms)^SUBDIRS = (.*?)(?=^DISABLED_SUBDIRS =)', text)
assert match, 'expected Wine SUBDIRS in config.status'
subdirs = match[1].split()
# Import libraries need only Wine's specs; avoid scanning resources omitted
# by Debian's repack in modules that we are not rebuilding.
subdirs = [d for d in subdirs if d not in ['dlls/' + m for m in modules]
           and d not in ('dlls/uwpdesktop-import-user32', 'dlls/uwpdesktop-import-win32u')]
for d in ['dlls/uwpdesktop', 'dlls/win32u', 'dlls/user32', 'libs/png', 'libs/zlib', 'libs/uuid']:
    if d not in subdirs:
        subdirs.append(d)
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
# Match Debian's win32u.so: FreeType/fontconfig/OSMesa/Vulkan are dlopened by
# soname (strings in /usr/lib/x86_64-linux-gnu/wine/x86_64-unix/win32u.so).
text = re.sub(r'(?m)^FREETYPE_CFLAGS = .*$', f'FREETYPE_CFLAGS = -I{include}/freetype2', text)
text = re.sub(r'(?m)^FONTCONFIG_CFLAGS = .*$', f'FONTCONFIG_CFLAGS = -I{include}', text)
defines = {'HAVE_FREETYPE': '1', 'HAVE_FT2BUILD_H': '1', 'HAVE_FT_TRUETYPEENGINETYPE': '1',
           'SONAME_LIBFREETYPE': '"libfreetype.so.6"', 'SONAME_LIBFONTCONFIG': '"libfontconfig.so.1"',
           'SONAME_LIBOSMESA': '"libOSMesa.so.8"', 'SONAME_LIBVULKAN': '"libvulkan.so.1"'}
for key, value in defines.items():
    line = 'D["%s"]=" %s"' % (key, value.replace('"', '\\"'))
    if line not in text:
        anchor = text.index('D["PACKAGE_NAME"]')
        text = text[:anchor] + line + '\n' + text[anchor:]
path.write_text(text)
PY
cd "$build"
./config.status > desk-configure.log 2>&1 || { tail -40 desk-configure.log >&2; exit 1; }
for key in HAVE_FREETYPE SONAME_LIBFREETYPE SONAME_LIBFONTCONFIG SONAME_LIBOSMESA SONAME_LIBVULKAN; do
    grep -q "^#define $key " include/config.h
done
# One make for the whole ABI set: the callback id is shared by all three.
targets=(dlls/uwpdesktop/x86_64-windows/uwpdesktop.dll dlls/user32/x86_64-windows/user32.dll
    dlls/win32u/x86_64-windows/win32u.dll dlls/win32u/win32u.so)
# Debian install dirs, as for ntdll in prepare-wine-wx.sh.
make -j"$(nproc)" bindir=/usr/lib/wine libdir=/usr/lib/x86_64-linux-gnu/wine datadir=/usr/share/wine \
    "${targets[@]}" > desk-build.log 2>&1 || { tail -40 desk-build.log >&2; exit 1; }
mkdir -p "$(dirname "$staged")"
cp dlls/uwpdesktop/x86_64-windows/uwpdesktop.dll "$staged"
# The loader's Debian-layout DLL directory also exposes the private module.
cp dlls/uwpdesktop/x86_64-windows/uwpdesktop.dll "$lib/x86_64-windows/uwpdesktop.dll"
# Replace the installed-Wine symlinks with the rebuilt, matching set.
for f in user32/x86_64-windows/user32.dll win32u/x86_64-windows/win32u.dll win32u/win32u.so; do
    dest=$lib/x86_64-windows/${f##*/}
    [[ $f == *.so ]] && dest=$lib/x86_64-unix/${f##*/}
    rm -f "${dest:?}"
    cp "dlls/$f" "$dest"
done
printf '%s\n' "$stamp" > "$tree/.desk-stamp"
echo "prepare-wine-desk: built $staged and user32.dll, win32u.dll, win32u.so"
