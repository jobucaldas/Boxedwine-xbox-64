#!/usr/bin/env python3
"""B61: mark a PE as an AppContainer image (IMAGE_DLL_CHARACTERISTICS_APPCONTAINER 0x1000)
and check the Xbox UWP activation contract: GUI subsystem >= 6.02, no desktop CRT DLL.
GNU ld has no /appcontainer switch; this sets the same header bit MSVC/lld /appcontainer
sets. It only declares the image runs inside the app's own AppContainer (more restricted),
it does not bypass anything. Without it the console fails activation as 0x8027025B.
Usage: pe-appcontainer.py [--check-only] EXE..."""
import struct, sys
FORBIDDEN = {b'msvcrt.dll', b'msvcp140.dll', b'vcruntime140.dll', b'vcruntime140_1.dll'}
def run(path, check_only):
    b = bytearray(open(path, 'rb').read())
    pe = struct.unpack_from('<I', b, 0x3c)[0]
    assert b[pe:pe+4] == b'PE\0\0', 'not a PE'
    opt = pe + 24
    assert struct.unpack_from('<H', b, opt)[0] == 0x20b, 'not PE32+'
    major, minor = struct.unpack_from('<HH', b, opt + 48)
    subsys, dllch = struct.unpack_from('<HH', b, opt + 68)
    errs = []
    if subsys != 2: errs.append(f'subsystem {subsys} (need 2 = Windows GUI)')
    if (major, minor) < (6, 2): errs.append(f'subsystem version {major}.{minor:02d} (need >= 6.02)')
    low = bytes(b).lower()
    for d in FORBIDDEN:
        if b'\0' + d + b'\0' in low: errs.append(f'imports desktop CRT {d.decode()}')
    if not dllch & 0x1000:
        if check_only: errs.append('no APPCONTAINER bit (0x1000)')
        else:
            struct.pack_into('<H', b, opt + 70, dllch | 0x1000)
            struct.pack_into('<I', b, opt + 64, 0)   # CheckSum: not validated for user-mode exes
            open(path, 'wb').write(b)
    print(f'{path}: ' + ('OK' if not errs else 'FAIL: ' + '; '.join(errs)))
    return not errs
args = sys.argv[1:]; co = '--check-only' in args
ok = all([run(p, co) for p in args if p != '--check-only'])
sys.exit(0 if ok else 1)
