#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds gpu/third_party/button-prompts/xbox.bin from the Xbox prompts mod, and prints the
XXH3 hashes of the game's own glyphs that gpu/shim/engine/button_prompts.cpp recognises.

    button_prompts.py <CUSA03173>/dvdroot_ps4/menu/common.tpf.dcx <mod>/menu/common.tpf.dcx

Development only (needs Pillow and xxhash): the output is checked in. The mod is Dommo's
"Xbox (and switch) UI for Bloodborne" (nexusmods.com/bloodborne/mods/30), modern Elden Ring
variant; its assets are free to use.
"""
import io
import struct
import sys
from pathlib import Path

import xxhash
from PIL import Image

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'scripts'))
from engine_assets import dcx_unpack  # noqa: E402

# The order button_prompts.cpp keeps them in.
KG = ('KG_Cancel', 'KG_L1', 'KG_L2', 'KG_L3', 'KG_LS', 'KG_L_D', 'KG_L_LR', 'KG_L_U', 'KG_L_UD',
      'KG_L_UDLR', 'KG_OK', 'KG_R1', 'KG_R2', 'KG_R3', 'KG_RS', 'KG_R_L', 'KG_R_U', 'KG_Start',
      'KG_TP_L', 'KG_TP_R')
ATLAS = 'MENU_Common_00091'
# The HUD's Circle, Cross and L3 in the atlas (1024x512): x, y, w, h, on 4x4 blocks.
SPOTS = ((660, 120, 28, 32), (692, 120, 28, 32), (724, 120, 28, 32))


def textures(path):
    raw = dcx_unpack(Path(path).read_bytes())
    assert raw[:4] == b'TPF\0' and raw[0xc] == 4, 'not a PS4 TPF'
    out = {}
    for i in range(struct.unpack_from('<I', raw, 8)[0]):
        off, size, _, _, mips, _, w, h, _, tile, name, _, dxgi = struct.unpack_from('<IIBBBBHHIIIII', raw, 0x10 + i * 0x24)
        end = name
        while raw[end:end + 2] != b'\0\0':
            end += 2
        out[raw[name:end].decode('utf-16le')] = (w, h, dxgi, tile, mips, raw[off:off + size])
    return out


def morton(x, y):
    return (x & 1) | (y & 1) << 1 | (x & 2) << 1 | (y & 2) << 2 | (x & 4) << 2 | (y & 4) << 3


def image(w, h, dxgi, data):
    """Thin 1D tiled BC (tile mode 13: 8x8-block micro tiles, Morton inside) to RGBA."""
    bw, bh = w // 4, h // 4
    linear = bytearray(len(data))
    for by in range(bh):
        for bx in range(bw):
            src = ((by // 8 * (bw // 8) + bx // 8) * 64 + morton(bx % 8, by % 8)) * 16
            dst = (by * bw + bx) * 16
            linear[dst:dst + 16] = data[src:src + 16]
    head = bytearray(128)
    head[:4] = b'DDS '
    struct.pack_into('<7I', head, 4, 124, 0x81007, h, w, len(data), 0, 1)
    struct.pack_into('<II4s', head, 76, 32, 4, b'DX10')
    struct.pack_into('<I', head, 108, 0x1000)
    dds = bytes(head) + struct.pack('<5I', dxgi, 3, 0, 1, 0) + bytes(linear)
    return Image.open(io.BytesIO(dds)).convert('RGBA')


def main(game, mod):
    original, xbox = textures(game), textures(mod)
    for name in KG:
        w, h, dxgi, tile, mips, data = original[name]
        assert (w, h, dxgi, tile, mips) == (32, 32, 98, 13, 1) == xbox[name][:5], name
        print(f'    0x{xxhash.xxh3_64_intdigest(data):016x}ull, // {name}')
    w, h, dxgi, tile, mips, data = original[ATLAS]
    assert (w, h, dxgi, tile, mips) == (1024, 512, 98, 13, 1), ATLAS
    print(f'    0x{xxhash.xxh3_64_intdigest(data):016x}ull, // {ATLAS}')
    # The mod's atlas is drawn at twice the size: brought back to the game's.
    atlas = image(*xbox[ATLAS][:3], xbox[ATLAS][5]).resize((1024, 512), Image.LANCZOS)
    blob = b''.join(xbox[name][5] for name in KG)
    blob += b''.join(atlas.crop((x, y, x + w, y + h)).tobytes() for x, y, w, h in SPOTS)
    out = Path(__file__).resolve().parents[2] / 'gpu/third_party/button-prompts/xbox.bin'
    out.parent.mkdir(exist_ok=True)
    out.write_bytes(blob)
    print(out, len(blob))


if __name__ == '__main__':
    main(*sys.argv[1:3])
