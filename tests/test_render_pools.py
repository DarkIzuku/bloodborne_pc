from paths import ROOT
import hashlib
import struct
import unittest
from unittest.mock import patch
import render_pools as pools

class RenderPools(unittest.TestCase):
    def setUp(self):
        self.elf=bytearray(1024)
        struct.pack_into('<Q',self.elf,0x20,64)
        struct.pack_into('<HH',self.elf,0x36,56,1)
        struct.pack_into('<IIQQQQQQ',self.elf,64,1,5,256,pools.SITES[0],0,512,512,4096)
        for at in pools.SITES:
            start=256+at-pools.SITES[0]
            self.elf[start:start+5]=pools.ORIGINAL
        self.allow=patch.object(pools,'EBOOT_SHA',{hashlib.sha256(self.elf).hexdigest()})
        self.allow.start();self.addCleanup(self.allow.stop)

    def test_two_immediates_only_and_source_unchanged(self):
        before=bytes(self.elf)
        writes=pools.render_pool_writes(self.elf,env={})
        self.assertEqual(writes,[(at+1,struct.pack('<I',1024*1024))for at in pools.SITES])
        self.assertEqual(bytes(self.elf),before)

    def test_foreign_version_and_truncated_elf_refused(self):
        for content in (b'',b'ELF',self.elf[:-1]):
            self.assertEqual(pools.render_pool_writes(content,env={}),[])

    def test_disabled(self):
        self.assertEqual(pools.render_pool_writes(self.elf,env={'BB_RENDER_POOLS':'0'}),[])

    def test_split_xml_overlap_including_opcode_refuses_whole_fix(self):
        for at in pools.SITES:
            for offset in range(at,at+5):
                self.assertEqual(pools.render_pool_writes(self.elf,[(offset,b'X')],{}),[])
        self.assertEqual(len(pools.render_pool_writes(self.elf,[(pools.SITES[0]-1,b'X')],{})),2)

    def test_nonexecuting_segment_cannot_be_patched(self):
        struct.pack_into('<I',self.elf,68,4)
        with patch.object(pools,'EBOOT_SHA',{hashlib.sha256(self.elf).hexdigest()}):
            self.assertEqual(pools.render_pool_writes(self.elf,env={}),[])

    def test_matching_profile_but_altered_instruction_refused(self):
        self.elf[256]=0x90
        with patch.object(pools,'EBOOT_SHA',{hashlib.sha256(self.elf).hexdigest()}):
            self.assertEqual(pools.render_pool_writes(self.elf,env={}),[])

if __name__=='__main__':unittest.main()
