"""Bounds and metadata checks for the public Intel firmware container reader."""
import importlib.util
from pathlib import Path
import struct
import unittest
spec=importlib.util.spec_from_file_location('wifi',Path(__file__).resolve().parents[1]/'wifi_firmware.py')
wifi=importlib.util.module_from_spec(spec);spec.loader.exec_module(wifi)


def record(kind,payload):
    return struct.pack('<II',kind,len(payload))+payload+bytes(-len(payload)%4)


class Firmware(unittest.TestCase):
    def setUp(self):
        self.header=wifi.HEADER.pack(0,0x0a4c5749,b'test',89,1,0)
        self.records=(record(19,struct.pack('<I',0x123400)+b'abc')+
                      record(36,struct.pack('<III',89,0x123cf747,0))+
                      record(48,bytes([7,2,3,4]))+record(999,b'future extension'))

    def test_metadata_and_unknown_records(self):
        report=wifi.inspect(self.header+self.records)
        self.assertEqual(report['firmware_revisions'],[[89,0x123cf747,0]])
        self.assertEqual(report['sections'][0]['bytes'],3)
        self.assertEqual(report['commands'],[dict(command=7,group=2,version=3,notification=4)])
        self.assertEqual(report['tlv_counts'][999],1)

    def test_bad_headers_and_lengths(self):
        for data in (b'',self.header[:80],b'\1'+self.header[1:],self.header[:4]+b'BAD!'+self.header[8:],
                     self.header+b'123',self.header+struct.pack('<II',19,0xffffffff),
                     self.header+record(19,b'abc'),self.header+record(36,b'1234'),
                     self.header+record(48,b'123'),self.header+self.records+record(48,b'1234')):
            with self.subTest(size=len(data)),self.assertRaises(ValueError):wifi.inspect(data)

    def test_every_incomplete_record_boundary_rejects(self):
        item=record(19,b'12345')
        for n in range(1,len(item)):
            with self.subTest(cut=n),self.assertRaises(ValueError):wifi.inspect(self.header+item[:n])


if __name__=='__main__':unittest.main()
