from paths import ROOT
import unittest
import struct
import engine_assets
import tempfile
from pathlib import Path
from unittest.mock import patch
from types import SimpleNamespace


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

    def test_menu_cache_rebuilds_corrupt_output_and_changed_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            movie = game / 'dvdroot_ps4/menu/optionsetting.gfx'
            messages = game / 'dvdroot_ps4/msg/engus/menu.msgbnd.dcx'
            for p in (movie, messages):
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'own dump')
            tool = root / 'tool'
            tool.write_bytes(b'generator version 1')
            calls = []
            def generate(args, **kwargs):
                calls.append(args[1])
                Path(args[3]).write_bytes(Path(args[2]).read_bytes() + b' prepared')
                return SimpleNamespace(returncode=0, stderr='')
            with patch.object(engine_assets, 'verify_executable'), patch.object(engine_assets.subprocess, 'run', generate):
                cache = engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 2)
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 2)
                cached = cache / movie.relative_to(game)
                cached.write_bytes(b'corrupt')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 3)
                self.assertEqual(movie.read_bytes(), b'own dump')
                messages.write_bytes(b'updated language')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 4)
                tool.write_bytes(b'generator version 2')
                engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
                self.assertEqual(len(calls), 6)

    def test_menu_generation_failure_does_not_publish_manifest_or_change_source(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            game = root / 'game'
            for name in ('menu/optionsetting.gfx', 'msg/engus/menu.msgbnd.dcx'):
                p = game / 'dvdroot_ps4' / name
                p.parent.mkdir(parents=True, exist_ok=True)
                p.write_bytes(b'original')
            tool = root / 'tool'
            tool.write_bytes(b'tool')
            with patch.object(engine_assets, 'verify_executable'), patch.object(engine_assets.subprocess, 'run',
                    return_value=SimpleNamespace(returncode=1, stderr='unsupported movie')):
                with self.assertRaisesRegex(ValueError, 'unsupported movie'):
                    engine_assets.prepare_menu_assets(game, root / 'eboot', root / 'cache', tool)
            self.assertFalse((root / 'cache/pc-menus/manifest.json').exists())
            self.assertTrue(all(p.read_bytes() == b'original' for p in game.rglob('*') if p.is_file()))


if __name__ == '__main__':
    unittest.main()
