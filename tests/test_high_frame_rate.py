from paths import ROOT
import hashlib
import struct
import unittest
from unittest.mock import patch
from frame_rate import STEPS, BASE, SLOT, adapt_high_frame_rate, scaled_values, replace_range
from patches import FPS_PRESETS, compile_patches

class HighFrameRateTests(unittest.TestCase):
    def setUp(self):
        self.writes=compile_patches(ROOT/'patches/Bloodborne.xml',FPS_PRESETS['120'],'01.09',[(0,0x6000000)])
        self.elf=b'synthetic executable for guarded patch tests'
        self.old={a-BASE:bytes.fromhex(old) for a,old,new in STEPS}
        self.old[0x1CBDB42-BASE]=bytes.fromhex('c5fa1083e0000000c5fa11857cffffff')
    def adapt(self,mode):
        with patch('frame_rate.EBOOT_SHA',{hashlib.sha256(self.elf).hexdigest()}),patch('frame_rate.original_bytes',side_effect=lambda e,a,n:self.old.get(a)):
            return adapt_high_frame_rate(self.writes,mode,self.elf)
    def bytes_at(self,writes,va,n):
        result={a+i:v for a,b in writes for i,v in enumerate(b)}
        return bytes(result[va-BASE+i] for i in range(n))
    def test_120_targets_and_character_time(self):
        writes=self.adapt('120')
        for a,b in scaled_values().items():self.assertEqual(self.bytes_at(writes,a,len(b)),b)
        for a,old,new in STEPS:self.assertEqual(self.bytes_at(writes,a,len(bytes.fromhex(new))),bytes.fromhex(new))
    def test_uncapped_uses_same_frame_jobs_and_measured_character_step(self):
        writes=self.adapt('uncap')
        for a in (0x2434840,0x243485A,0x243487E,0x24348B3):self.assertEqual(self.bytes_at(writes,a+5,4),bytes(4))
        self.assertEqual(FPS_PRESETS['uncap'],FPS_PRESETS['120'])
        data=self.bytes_at(writes,0x1CBDB42,16)
        self.assertEqual(data[:4],bytes.fromhex('c5fa1005'))
        self.assertEqual(0x1CBDB42+8+struct.unpack_from('<i',data,4)[0],SLOT)
        self.assertEqual(data[8:],bytes.fromhex('c5fa11857cffffff'))
    def test_unverified_image_is_rejected(self):
        with self.assertRaisesRegex(ValueError,'verified'):adapt_high_frame_rate(self.writes,'120',self.elf)
    def test_each_mismatched_timing_site_is_rejected(self):
        for a in self.old:
            with self.subTest(address=hex(a)):
                old=self.old[a];self.old[a]=bytes(len(old))
                with self.assertRaises(ValueError):self.adapt('uncap')
                self.old[a]=old
    def test_changed_community_layout_is_rejected(self):
        self.writes=[(a,b) for a,b in self.writes if a!=0xF383CA-BASE]
        with self.assertRaisesRegex(ValueError,'missing'):self.adapt('120')
    def test_range_replacement_keeps_neighbours(self):
        self.assertEqual(replace_range([(10,b'abcdef')],BASE+12,b'XY'),[(10,b'ab'),(14,b'ef'),(12,b'XY')])
    def test_ordinary_presets_are_not_modified(self):
        for mode in ('30','60','90'):self.assertIs(adapt_high_frame_rate(self.writes,mode,b''),self.writes)

if __name__=='__main__':unittest.main()
