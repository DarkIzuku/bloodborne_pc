from paths import ROOT
import unittest
import struct
import engine_assets


class EngineAssetTests(unittest.TestCase):
    def test_dcx_roundtrip(self):
        raw = b'MSB fixture\0' * 500
        packed = engine_assets.dcx_pack(raw)
        self.assertEqual(len(packed[:0x4c]), 0x4c)
        self.assertEqual(engine_assets.dcx_unpack(packed), raw)

    def test_bad_size_or_truncated_stream_is_rejected(self):
        packed = bytearray(engine_assets.dcx_pack(b'unchanged source' * 20))
        struct.pack_into('>I', packed, 0x1c, 1)
        with self.assertRaises(ValueError):
            engine_assets.dcx_unpack(packed)
        with self.assertRaises(ValueError):
            engine_assets.dcx_unpack(engine_assets.dcx_pack(b'valid')[:-4])

    def test_wrong_layout_does_not_mutate_input(self):
        raw = bytearray(b'not a supported Dream layout')
        before = bytes(raw)
        with self.assertRaisesRegex(ValueError, 'original 1.09'):
            engine_assets.mirror_layout(raw)
        self.assertEqual(raw, before)


if __name__ == '__main__':
    unittest.main()
