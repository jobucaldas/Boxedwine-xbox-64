#!/usr/bin/env python3
"""Build-only synthetic regression tests; no Valve code or credentials are executed."""
import copy
import importlib.util
import os
import shutil
import json
from pathlib import Path
import resource
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('fixtures', HERE.parent / 'ifacemap/test_validate.py')
f = importlib.util.module_from_spec(spec)
spec.loader.exec_module(f)


def run(*args, **kwargs):
    return subprocess.run(args, check=True, **kwargs)


with tempfile.TemporaryDirectory(prefix='denylist-') as tmp:
    tmp = Path(tmp)
    unit = str(tmp / 'unit')
    run('cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-I' + str(HERE),
        str(HERE / 'test-denylist.c'), '-o', unit)
    run(unit)
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    trapped = subprocess.run([unit], env={**os.environ, 'TEST_TRAP': '1'}, capture_output=True)
    assert trapped.returncode < 0 and b'denied_slot=Unknown' in trapped.stderr
    mapping, data = f.fixture()
    f.v.PIN = mapping['steamclient64_sha256']
    slot = mapping['interfaces']['IClientUtils']['slots'][0]
    for name in ('InvalidateCredentials', 'DestroyCachedCredentials', 'LogOffAll',
                 'RevokeRefreshToken', 'ForgetCredential', 'clear-login-users'):
        slot.update(name=name, evidence='credential revoke invalidate logoff')
        f.v.validate(mapping, data)
        mutant = copy.deepcopy(mapping)
        mutant['interfaces']['IClientUtils']['slots'][0]['name'] = 'SafeRenamedMethod'
        try:
            f.v.validate(mutant, data)
        except f.v.InvalidMap as error:
            assert str(error) == 'renamed_denied_slot'
        else:
            raise AssertionError('renamed denied slot accepted')
    print('unit_trap=pass mutated_map=refused (synthetic PE)')
    # Build the actual host source against a synthetic generated map, then disassemble it.
    spec = importlib.util.spec_from_file_location('generator', HERE.parent / 'ifacemap/generate-host.py')
    generator = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(generator)
    (tmp / 'out').mkdir()
    header = generator.generate(mapping, json.dumps(mapping).encode())
    assert '{"IClientUtils","clear-login-users",0u,1,(void *)denied_trap}' in header
    (tmp / 'out/host-map.h').write_text(header)
    shutil.copy(HERE / 'steamhost.c', tmp / 'steamhost.c')
    pe = tmp / 'steamhost64.exe'
    run('x86_64-w64-mingw32-gcc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror', '-I' + str(HERE),
        '-DPIN_VERSION=1788652215u', '-DPIN_SHA256=\"synthetic\"',
        str(tmp / 'steamhost.c'), '-o', str(pe), '-lbcrypt')
    disassembly = subprocess.check_output(['objdump', '-d', str(pe)]).lower()
    # The synthetic fixture's denied function VA/RVA must not appear as a target.
    for address in (b'180001010', b'0x1010'):
        assert address not in disassembly, 'denied synthetic target in PE disassembly'
    print('static_denied_rva=absent (synthetic PE; production map unavailable)')
# When staged, require the production map and disassemble the production host too.
production_map = HERE.parents[2] / 'out/ifacemap-1788652215.json'
client = HERE / 'out/client/steamclient64.dll'
if production_map.exists() and client.exists():
    run('make', '-C', str(HERE), 'all')
    mapping = json.loads(production_map.read_bytes())
    data = client.read_bytes()
    generator.v.validate(mapping, data)
    pe = generator.v.PE(data)
    disassembly = subprocess.check_output(['objdump', '-d', str(HERE / 'out/steamhost64.exe')]).lower()
    for table in mapping['interfaces'].values():
        for slot in table['slots']:
            if generator.denied(slot):
                address = pe.pointer(generator.v.number(table['vtable_rva']) + generator.v.number(slot['index']) * 8)
                for value in (address, address - pe.base):
                    assert format(value, 'x').encode() not in disassembly, 'denied production address'
    print('production_static_denied_rva=absent')
print('DENYLIST PASS')
