from paths import ROOT
import struct
import tempfile
import unittest
import xml.etree.ElementTree as ET
from pathlib import Path

import patches

from patches import (EBOOT_BASE, OUTPUT_SIZE, RESOLUTION_TEMPLATE, SCENE_HEIGHT,
                     SCENE_WIDTH, UI_HEIGHT, UI_WIDTH, compile_patches,
                     render_size, resolution_writes, scaled_sizes, effect_patches,
                     validate_patch_requirements, external_patches, external_selection,
                     compile_external)

XML = ROOT / 'patches/Bloodborne.xml'
SEGMENTS = [(0, 0x6000000)]


def immediate(writes, address):
    # The XML splits a 9-byte MOV imm32 + NOP into 4+4+1 byte writes.
    data = bytearray(9)
    for offset, value in writes:
        if address <= offset < address + 9:
            data[offset-address:offset-address+len(value)] = value
    assert data[5:] == b'\x0f\x1f\x40\x00'
    return data[0], struct.unpack_from('<I', data, 1)[0]


class NativeUiTests(unittest.TestCase):
    def test_presets_keep_ui_native(self):
        for preset, expected in [(1, (1280, 720)), (2, (1130, 636)),
                                 (3, (960, 540)), (4, (640, 360))]:
            with self.subTest(preset=preset):
                size = render_size({'upscaler': 'fsr3', 'preset': str(preset)})
                self.assertEqual(size, expected)
                writes = resolution_writes(XML, size, '01.09', SEGMENTS)
                self.assertEqual(immediate(writes, SCENE_WIDTH), (0xB8, size[0]))
                self.assertEqual(immediate(writes, SCENE_HEIGHT), (0xB8, size[1]))
                self.assertEqual(immediate(writes, UI_WIDTH), (0xB8, OUTPUT_SIZE[0]))
                self.assertEqual(immediate(writes, UI_HEIGHT), (0xB9, OUTPUT_SIZE[1]))

    def test_explicit_scene_resolution_keeps_native_ui(self):
        writes = resolution_writes(XML, (800, 450), '01.09', SEGMENTS)
        self.assertEqual(immediate(writes, SCENE_WIDTH)[1], 800)
        self.assertEqual(immediate(writes, UI_WIDTH)[1], 1920)
        self.assertEqual(immediate(writes, UI_HEIGHT)[1], 1080)

    def test_coordinate_and_aspect_fixes_are_preserved(self):
        original = compile_patches(XML, [RESOLUTION_TEMPLATE], '01.09', SEGMENTS)
        modified = resolution_writes(XML, (960, 540), '01.09', SEGMENTS)
        self.assertEqual(len(original), len(modified))
        changed = {SCENE_WIDTH, SCENE_HEIGHT, UI_WIDTH, UI_HEIGHT}
        self.assertEqual([w for w in original if w[0] not in changed],
                         [w for w in modified if w[0] not in changed])

    def test_unexpected_or_missing_ui_instruction_is_rejected(self):
        for remove in (False, True):
            tree = ET.parse(XML)
            for meta in tree.getroot().iter('Metadata'):
                if meta.get('Name') == RESOLUTION_TEMPLATE and meta.get('AppVer') == '01.09':
                    patch_list = meta.find('PatchList')
                    for line in list(patch_list):
                        if int(line.get('Address'), 0) == UI_WIDTH + EBOOT_BASE:
                            if remove:
                                patch_list.remove(line)
                            else:
                                line.set('Value', '0x000500B9')
            with tempfile.TemporaryDirectory() as directory:
                path = Path(directory) / 'patch.xml'
                tree.write(path)
                with self.assertRaises(ValueError):
                    resolution_writes(path, (960, 540), '01.09', SEGMENTS)

    def test_native_and_disabled_upscaler_need_no_resolution_patch(self):
        self.assertIsNone(render_size({'preset': '0'}))
        self.assertIsNone(render_size({'upscaler': 'off', 'preset': '3'}))

    def test_output_other_than_1080p_scales_the_scene(self):
        self.assertIsNone(scaled_sizes({'output_res': '1920x1080', 'preset': '2'}))
        # Steam Deck: below 1080p the scene is still the preset's fraction of the output.
        for preset, expected in [(0, (1280, 720)), (2, (752, 424)), (4, (426, 240))]:
            with self.subTest(preset=preset):
                self.assertEqual(scaled_sizes({'output_res': '1280x720', 'preset': str(preset)}),
                                 (expected, (1280, 720)))
        self.assertEqual(scaled_sizes({'output_res': '3840x2160', 'preset': '3'}),
                         ((1916, 1078), (3840, 2160)))
        # TAA is native-only and uses the live host targets.
        self.assertIsNone(scaled_sizes({'output_res': '1280x720', 'upscaler': 'taa', 'preset': '3'}))


class DebugPatchTests(unittest.TestCase):
    def test_camera_patch_is_optional_and_compatible_with_fps_and_debug_menu(self):
        self.assertEqual(effect_patches({'debug_camera': '0', 'debug_menu': '0'}), [])
        camera = effect_patches({'debug_camera': '1'})
        writes = compile_patches(XML, camera, '01.09', SEGMENTS)
        self.assertGreater(len(writes), 0)
        camera_bytes = {offset+i: byte for offset, data in writes for i, byte in enumerate(data)}
        for patch in ('Uncap FPS++', '60 FPS++', '90 FPS++', 'Restore Debug Menu (READ NOTES)'):
            for offset, data in compile_patches(XML, [patch], '01.09', SEGMENTS):
                for i, byte in enumerate(data):
                    if offset+i in camera_bytes:
                        self.assertEqual(camera_bytes[offset+i], byte, patch)

    def test_debug_menu_checks_both_fonts_but_camera_does_not_need_them(self):
        names = effect_patches({'debug_menu': '1'})
        with tempfile.TemporaryDirectory() as directory:
            game = Path(directory)
            validate_patch_requirements(['Restore Debug Camera'], game)
            with self.assertRaisesRegex(ValueError, 'DbgFont14h.ccm.*DbgFont14h.tpf'):
                validate_patch_requirements(names, game)
            font = game / 'dvdroot_ps4/font'
            font.mkdir(parents=True)
            (font / 'DbgFont14h.ccm').write_bytes(b'test')
            (font / 'DbgFont14h.tpf').touch()
            with self.assertRaisesRegex(ValueError, 'DbgFont14h.tpf'):
                validate_patch_requirements(names, game)
            (font / 'DbgFont14h.tpf').write_bytes(b'test')
            validate_patch_requirements(names, game)

    def test_conflicting_enemy_control_patch_is_rejected(self):
        with self.assertRaisesRegex(ValueError, 'conflicts with Enemy Control'):
            validate_patch_requirements(['Enemy Control', 'Restore Debug Camera'], Path('.'))


EXTERNAL = """<?xml version="1.0"?>
<Patch>
  <TitleID><ID>CUSA03173</ID><ID>CUSA00207</ID></TitleID>
  <Metadata Title="Bloodborne" Name="On" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00401000" Value="9090"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Off" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin">
    <PatchList><Line Type="bytes32" Address="0x00402000" Value="0x12345678"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Mask" Author="x" PatchVer="1.0" AppVer="01.09" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="mask" Value="90 ?? 90" Offset="0"/></PatchList>
  </Metadata>
  <Metadata Title="Bloodborne" Name="Old" Author="x" PatchVer="1.0" AppVer="01.00" AppElf="eboot.bin" isEnabled="true">
    <PatchList><Line Type="bytes" Address="0x00403000" Value="90"/></PatchList>
  </Metadata>
</Patch>"""


class ExternalPatchTests(unittest.TestCase):
    def test_selection_and_unsupported_lines(self):
        with tempfile.TemporaryDirectory() as directory:
            folder = Path(directory)
            (folder / 'extra.xml').write_text(EXTERNAL)
            (folder / 'other.xml').write_text(EXTERNAL.replace('CUSA03173', 'CUSA99999')
                                              .replace('CUSA00207', 'CUSA99998'))
            (folder / 'broken.xml').write_text('<Patch>')
            found = external_patches(folder)
            self.assertEqual([key for key, _, _ in found],
                             ['extra.xml/On', 'extra.xml/Off', 'extra.xml/Mask'])
            # The file's isEnabled; the mask patch is skipped as a whole.
            writes = compile_external(external_selection(found, None), SEGMENTS)
            self.assertEqual(writes, [(0x1000, bytes.fromhex('9090'))])
            config = folder / 'patches.json'
            config.write_text('{"enabled": ["extra.xml/Off"], "disabled": ["extra.xml/On"]}')
            writes = compile_external(external_selection(found, config), SEGMENTS)
            self.assertEqual(writes, [(0x2000, (0x12345678).to_bytes(4, 'little'))])

    def test_built_in_file_is_not_external(self):
        self.assertEqual(external_patches(XML.parent), [])


def tiny_elf(code, vaddr=0x1000):
    """An ELF with one load segment holding `code` at `vaddr`."""
    header = bytearray(0x100)
    header[:7] = b'\x7fELF\x02\x01\x01'
    struct.pack_into('<H', header, 18, 62)
    struct.pack_into('<H', header, 0x36, 56)
    struct.pack_into('<Q', header, 0x20, 0x40)
    struct.pack_into('<H', header, 0x38, 1)
    struct.pack_into('<IIQQQQQQ', header, 0x40, 1, 5, 0x100, vaddr, vaddr, len(code), len(code), 0x1000)
    return bytes(header) + code


class ModEbootTests(unittest.TestCase):
    def test_in_place_changes_become_writes(self):
        code = bytes(0x30000)
        modified = bytearray(code)
        modified[5:7] = b'\xaa\xbb'
        modified[0x1effe:0x1f002] = b'\x01\x02\x03\x04'  # image 0x1fffe: crosses a diff chunk
        writes = patches.eboot_mod_writes(tiny_elf(code), tiny_elf(bytes(modified)))
        self.assertEqual(writes, [(0x1005, b'\xaa\xbb'), (0x1fffe, b'\x01\x02'), (0x20000, b'\x03\x04')])
        self.assertEqual(patches.eboot_mod_writes(tiny_elf(code), tiny_elf(code)), [])

    def test_other_layout_or_format_rejected(self):
        with self.assertRaisesRegex(ValueError, 'segment layout'):
            patches.eboot_mod_writes(tiny_elf(bytes(64)), tiny_elf(bytes(65)))
        with self.assertRaisesRegex(ValueError, 'plaintext'):
            patches.eboot_mod_writes(tiny_elf(bytes(64)), b'not an executable')

    def test_enabled_mods_contribute_xml_and_eboot(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            game, mods = root / 'game', root / 'mods'
            game.mkdir()
            (game / 'eboot.bin').write_bytes(tiny_elf(bytes(16)))
            (mods / 'A' / 'xml').mkdir(parents=True)
            (mods / 'A' / 'eboot.bin').write_bytes(tiny_elf(b'\x90' + bytes(15)))
            (mods / 'A' / 'xml' / 'a.xml').write_text(EXTERNAL)
            (mods / 'B').mkdir()
            (mods / 'B' / 'b.xml').write_text(EXTERNAL)
            (mods / 'loose.xml').write_text(EXTERNAL)
            config = root / 'mods.json'
            config.write_text('{"disabled": ["B"]}')
            found, writes = patches.mod_patches(mods, config, '01.09', game, XML)
            self.assertEqual(sorted({key.split('/')[0] for key, _, _ in found}), ['mods'])
            self.assertEqual(writes, [(0x1000, b'\x90')])


class ModPatchSafetyTests(unittest.TestCase):
    def test_malformed_program_headers(self):
        for data in (b"", b"\\x7fELF", tiny_elf(b"a")[:70]):
            with self.assertRaises(ValueError): patches.eboot_mod_writes(tiny_elf(b"a"), data)

    def test_no_load_segments_and_oversized_address(self):
        bad=bytearray(tiny_elf(b"a"));struct.pack_into('<I',bad,0x40,0)
        with self.assertRaises(ValueError): patches.memory_image(bytes(bad))
        with self.assertRaises(ValueError): patches.memory_image(tiny_elf(b"a",vaddr=512*1024*1024))

    def test_identical_filenames_have_distinct_mod_keys(self):
        with tempfile.TemporaryDirectory() as d:
            root=Path(d);game=root/'game';game.mkdir()
            (game/'eboot.bin').write_bytes(tiny_elf(bytes(16)))
            for name in ('A','B'):
                folder=root/'mods'/name;folder.mkdir(parents=True)
                (folder/'same.xml').write_text(EXTERNAL)
            found,_=patches.mod_patches(root/'mods',None,'01.09',game,XML)
            self.assertEqual(len({key for key,_,_ in found}),len(found))
            self.assertEqual({key.split('/')[1] for key,_,_ in found},{'A','B'})
            key=found[0][0];config=root/'patches.json'
            config.write_text(__import__('json').dumps({'enabled':[key], 'disabled':[x[0] for x in found if x[0]!=key]}))
            self.assertEqual([x[0] for x in patches.external_selection(found,config)],[key])


class IntelTonemapTests(unittest.TestCase):
    def cpuinfo(self, vendor):
        path = Path(tempfile.mkdtemp()) / 'cpuinfo'
        path.write_text(f'processor\t: 0\nvendor_id\t: {vendor}\nmodel name\t: x\n')
        return str(path)

    def test_on_for_intel_off_for_amd(self):
        self.assertTrue(patches.intel_tonemap_fix({}, self.cpuinfo('GenuineIntel')))
        self.assertFalse(patches.intel_tonemap_fix({}, self.cpuinfo('AuthenticAMD')))

    def test_environment_forces_it(self):
        amd, intel = self.cpuinfo('AuthenticAMD'), self.cpuinfo('GenuineIntel')
        self.assertTrue(patches.intel_tonemap_fix({'BB_INTEL_TONEMAP_FIX': '1'}, amd))
        self.assertFalse(patches.intel_tonemap_fix({'BB_INTEL_TONEMAP_FIX': '0'}, intel))

    def test_patch_exists_for_109(self):
        xml = ET.parse(ROOT / 'patches/Bloodborne.xml')
        names = [m.get('Name') for m in xml.iter('Metadata')]
        self.assertIn(patches.INTEL_TONEMAP, names)


if __name__ == '__main__':
    unittest.main()
