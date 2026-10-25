#!/usr/bin/env python3
"""Synthetic PE tests; never execute or modify a Valve binary."""
import copy
import hashlib
import importlib.util
from pathlib import Path
import struct
import unittest
from unittest.mock import patch

spec = importlib.util.spec_from_file_location('validator', Path(__file__).with_name('validate-map.py'))
v = importlib.util.module_from_spec(spec)
spec.loader.exec_module(v)


def fixture():
    data = bytearray(0x1000)
    data[:2] = b'MZ'
    struct.pack_into('<I', data, 0x3c, 0x80)
    data[0x80:0x84] = b'PE\0\0'
    struct.pack_into('<HH', data, 0x84, 0x8664, 2)
    struct.pack_into('<H', data, 0x94, 0xf0)
    struct.pack_into('<H', data, 0x98, 0x20b)
    struct.pack_into('<Q', data, 0xb0, 0x180000000)
    for i, (name, rva, raw) in enumerate(((b'.text', 0x1000, 0x400), (b'.rdata', 0x2000, 0x800))):
        offset = 0x188 + i * 40
        data[offset:offset + len(name)] = name
        struct.pack_into('<IIII', data, offset + 8, 0x400, rva, 0x400, raw)
    interfaces = {}
    for i, (name, methods) in enumerate(v.REQUIRED.items()):
        methods = sorted(methods) or ['Unknown']
        version = name + '001'
        offset = 0xc00 + i * 64
        data[offset:offset + len(version) + 1] = version.encode() + b'\0'
        rva = 0x2000 + i * 0x80
        for j in range(len(methods)):
            struct.pack_into('<Q', data, 0x800 + i * 0x80 + j * 8, 0x180001010)
        interfaces[name] = dict(version_string=version, vtable_rva=hex(rva), count=len(methods),
                                slots=[dict(index=j, name=m, confidence='exact') for j, m in enumerate(methods)])
    digest = hashlib.sha256(data).hexdigest()
    return dict(build_id='1788652215', steamclient64_sha256=digest, interfaces=interfaces), bytes(data)


class ValidateTests(unittest.TestCase):
    def setUp(self):
        self.mapping, self.data = fixture()
        self.pin = patch.object(v, 'PIN', self.mapping['steamclient64_sha256'])
        self.pin.start()
        self.addCleanup(self.pin.stop)

    def refuse(self, reason, mapping=None, data=None):
        with self.assertRaisesRegex(ValueError, reason):
            v.validate(mapping if mapping is not None else self.mapping,
                       data if data is not None else self.data)

    def test_valid(self):
        v.validate(self.mapping, self.data)

    def test_hash_mismatch(self):
        self.refuse('hash_mismatch', data=self.data + b'x')

    def test_map_cannot_choose_pin(self):
        with patch.object(v, 'PIN', '0' * 64):
            self.refuse('hash_mismatch')

    def test_missing_version_string(self):
        self.mapping['interfaces']['IClientUser']['version_string'] = 'Absent001'
        self.refuse('missing_version_string')

    def test_count_mismatch(self):
        for delta in (-1, 1):
            mapping = copy.deepcopy(self.mapping)
            mapping['interfaces']['IClientEngine']['count'] += delta
            self.refuse('count_mismatch', mapping=mapping)

    def test_low_confidence_called_slot(self):
        for confidence in ('positional', 'unknown', None):
            self.mapping['interfaces']['IClientUser']['slots'][0]['confidence'] = confidence
            self.refuse('low_confidence_called_slot')

    def test_missing_called_slot(self):
        self.mapping['interfaces']['IClientUser']['slots'][0]['name'] = 'Unknown'
        self.refuse('missing_called_slot')

    def test_callback_confidence(self):
        slot = self.mapping['interfaces']['IClientUtils']['slots'][0]
        slot.update(name='GetCallback', confidence='positional')
        self.refuse('low_confidence_called_slot')

    def test_explicit_host_call(self):
        self.mapping['interfaces']['IClientUtils']['slots'][0].update(host_called=True, confidence='unknown')
        self.refuse('low_confidence_called_slot')

    def test_rdata_required(self):
        self.mapping['interfaces']['IClientUser']['vtable_rva'] = 0x1000
        self.refuse('vtable_outside_rdata')

    def test_duplicate_index(self):
        self.mapping['interfaces']['IClientEngine']['slots'][1]['index'] = 0
        self.refuse('invalid_slot_index')


if __name__ == '__main__':
    unittest.main(verbosity=2)
