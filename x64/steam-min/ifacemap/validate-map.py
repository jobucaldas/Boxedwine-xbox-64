#!/usr/bin/env python3
"""Fail-closed, read-only validator for the single locally approved client map.

Callback pumping currently uses Steam_BGetCallback/Steam_FreeLastCallback exports,
not vtable slots. Any callback-named or explicitly host_called slot in a map is
nevertheless subject to the same confidence gate as the mandatory host methods.
"""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import sys
sys.path.insert(0, str(Path(__file__).resolve().parent))
from denylist import denied_text

PIN = 'caba4826aa3501039d095aee1843a6bfb270fb43a3ab4455b2d6733223579fee'
REQUIRED = {
    'IClientEngine': {'RunFrame', 'CreateGlobalUser', 'GetIClientUser',
                      'GetIClientUtils', 'GetIClientAppManager'},
    'IClientUser': {'LogOn'},
    'IClientUtils': set(),
    'IClientAppManager': set(),
}


class InvalidMap(ValueError):
    pass


def require(ok, reason):
    if not ok:
        raise InvalidMap(reason)


def number(value):
    require(type(value) is int or isinstance(value, str), 'invalid_integer')
    return int(value, 0) if isinstance(value, str) else value


class PE:
    def __init__(self, data):
        self.data = data
        require(data[:2] == b'MZ', 'invalid_dos_header')
        pe = self.unpack('<I', 0x3c)[0]
        require(data[pe:pe + 4] == b'PE\0\0', 'invalid_pe_signature')
        machine, count = self.unpack('<HH', pe + 4)
        size = self.unpack('<H', pe + 20)[0]
        opt = pe + 24
        require(machine == 0x8664 and size >= 112 and
                self.unpack('<H', opt)[0] == 0x20b, 'not_pe64')
        self.base = self.unpack('<Q', opt + 24)[0]
        self.sections = {}
        for i in range(count):
            off = opt + size + i * 40
            name = data[off:off + 8].rstrip(b'\0')
            virtual_size, rva, raw_size, raw = self.unpack('<IIII', off + 8)
            require(name not in self.sections, 'duplicate_section')
            require(raw + raw_size <= len(data), 'truncated_section')
            self.sections[name] = (rva, virtual_size, raw, raw_size)
        require(b'.text' in self.sections and b'.rdata' in self.sections,
                'missing_sections')

    def unpack(self, fmt, offset):
        require(0 <= offset <= len(self.data) - struct.calcsize(fmt), 'truncated_pe')
        return struct.unpack_from(fmt, self.data, offset)

    def contains(self, name, rva, size=1):
        start, virtual, _, _ = self.sections[name]
        return start <= rva and rva + size <= start + virtual

    def pointer(self, rva):
        require(self.contains(b'.rdata', rva, 8), 'vtable_outside_rdata')
        start, _, raw, length = self.sections[b'.rdata']
        require(rva - start + 8 <= length, 'vtable_not_file_backed')
        return self.unpack('<Q', raw + rva - start)[0]

    def text_pointer(self, rva):
        return self.contains(b'.text', self.pointer(rva) - self.base)


def validate(mapping, data):
    digest = hashlib.sha256(data).hexdigest()
    require(digest == mapping.get('steamclient64_sha256') == PIN, 'hash_mismatch')
    require(str(mapping.get('build_id')) == '1788652215', 'build_mismatch')
    pe = PE(data)
    interfaces = mapping['interfaces']
    require(isinstance(interfaces, dict) and REQUIRED.keys() <= interfaces.keys(),
            'missing_interfaces')
    for name, iface in interfaces.items():
        version = iface['version_string']
        require(isinstance(version, str) and version and '\0' not in version,
                'invalid_version_string')
        require(version.encode('ascii') + b'\0' in data, 'missing_version_string')
        rva, count = number(iface['vtable_rva']), number(iface['count'])
        require(count > 0 and rva % 8 == 0, 'invalid_vtable')
        require(pe.contains(b'.rdata', rva, count * 8), 'vtable_outside_rdata')
        for index in range(count):
            require(pe.text_pointer(rva + index * 8), 'count_mismatch')
        # A shorter claimed count must not hide more consecutive text pointers.
        if pe.contains(b'.rdata', rva + count * 8, 8):
            require(not pe.text_pointer(rva + count * 8), 'count_mismatch')
        slots = iface['slots']
        require(isinstance(slots, list) and len(slots) == count, 'slot_count_mismatch')
        indices, names = set(), set()
        required = REQUIRED.get(name, set())
        for slot in slots:
            index = number(slot['index'])
            method = slot['name']
            require(0 <= index < count and index not in indices, 'invalid_slot_index')
            require(isinstance(method, str), 'invalid_slot_name')
            indices.add(index)
            # Evidence must never silently rename a destructive entry to a safe method.
            require(not denied_text(slot.get('evidence', '')) or denied_text(method),
                    'renamed_denied_slot')
            if method in required:
                require(method not in names, 'duplicate_called_slot')
                names.add(method)
            called = (method in required or 'callback' in method.lower()
                      or slot.get('host_called', False))
            if called:
                require(slot.get('confidence') in ('exact', 'string-xref'),
                        'low_confidence_called_slot')
        require(required <= names, 'missing_called_slot')


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--map', required=True)
    parser.add_argument('--dll', required=True)
    try:
        args = parser.parse_args()
        validate(json.loads(Path(args.map).read_text()), Path(args.dll).read_bytes())
    except (Exception, SystemExit) as error:
        # Fixed diagnostics avoid echoing map contents or private file paths.
        reason = str(error) if isinstance(error, InvalidMap) else type(error).__name__
        print('map_valid=0 reason=' + reason.replace('\n', ' '))
        return 1
    print('map_valid=1')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
