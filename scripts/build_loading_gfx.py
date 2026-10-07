#!/usr/bin/env python3
"""Build Bloodborne PC's custom classic loading GFX from the user's own nowloading.gfx.

The original game file is read-only. Six packaged JPEG backgrounds are embedded into a generated
copy under out/, the original classic timeline/fades/loading indicator are preserved, and only its
background sprite is replaced. The selector is a tiny AVM2 MovieClip generated here so the
portable build has no external Flash compiler dependency.
"""
import argparse
import hashlib
from pathlib import Path
import struct

CLASS_NAME = 'BBRandomLoadingBackground'
EXPECTED_COUNT = 6


class BitWriter:
    def __init__(self):
        self.bits = []

    def u(self, value, count):
        for bit in range(count - 1, -1, -1):
            self.bits.append((value >> bit) & 1)

    def s(self, value, count):
        if value < 0:
            value = (1 << count) + value
        self.u(value, count)

    def finish(self):
        while len(self.bits) & 7:
            self.bits.append(0)
        out = bytearray()
        for offset in range(0, len(self.bits), 8):
            value = 0
            for bit in self.bits[offset:offset + 8]:
                value = (value << 1) | bit
            out.append(value)
        return bytes(out)


def signed_bits(*values):
    bits = 1
    while any(not (-(1 << (bits - 1)) <= value <= (1 << (bits - 1)) - 1) for value in values):
        bits += 1
    return bits


def rect(xmin, xmax, ymin, ymax):
    count = signed_bits(xmin, xmax, ymin, ymax)
    writer = BitWriter()
    writer.u(count, 5)
    for value in (xmin, xmax, ymin, ymax):
        writer.s(value, count)
    return writer.finish()


def matrix(scale_x=1.0, scale_y=1.0, tx=0, ty=0):
    writer = BitWriter()
    writer.u(1, 1)  # HasScale
    sx, sy = round(scale_x * 65536), round(scale_y * 65536)
    count = signed_bits(sx, sy)
    writer.u(count, 5)
    writer.s(sx, count)
    writer.s(sy, count)
    writer.u(0, 1)  # no rotate/skew
    count = signed_bits(tx, ty)
    writer.u(count, 5)
    writer.s(tx, count)
    writer.s(ty, count)
    return writer.finish()


def tag(code, payload=b''):
    length = len(payload)
    if length < 0x3F:
        return struct.pack('<H', (code << 6) | length) + payload
    return struct.pack('<HI', (code << 6) | 0x3F, length) + payload


def read_bits(data, bitpos, count):
    value = 0
    for _ in range(count):
        value = (value << 1) | ((data[bitpos >> 3] >> (7 - (bitpos & 7))) & 1)
        bitpos += 1
    return value, bitpos


def header_end(data):
    bitpos = 8 * 8
    nbits, bitpos = read_bits(data, bitpos, 5)
    bitpos += nbits * 4
    return (bitpos + 7) // 8 + 4


def parse_tag(data, pos):
    start = pos
    record = struct.unpack_from('<H', data, pos)[0]
    pos += 2
    code, length = record >> 6, record & 0x3F
    if length == 0x3F:
        length = struct.unpack_from('<I', data, pos)[0]
        pos += 4
    end = pos + length
    if end > len(data):
        raise ValueError('truncated GFX tag')
    return code, data[pos:end], start, end


def symbol_classes(payload):
    count = struct.unpack_from('<H', payload, 0)[0]
    result = []
    pos = 2
    for _ in range(count):
        character = struct.unpack_from('<H', payload, pos)[0]
        pos += 2
        end = payload.index(0, pos)
        result.append((character, payload[pos:end].decode('utf-8', errors='strict')))
        pos = end + 1
    return result


def add_symbol_class(payload, character, name):
    entries = symbol_classes(payload)
    if any(existing_name == name for _, existing_name in entries):
        raise ValueError(f'{name} already exists in SymbolClass')
    out = bytearray(struct.pack('<H', len(entries) + 1))
    # Preserve every original SymbolClass entry byte-for-byte after the count.
    out.extend(payload[2:])
    out.extend(struct.pack('<H', character))
    out.extend(name.encode('utf-8') + b'\0')
    return bytes(out)


def jpeg_dimensions(data):
    if not data.startswith(b'\xFF\xD8'):
        raise ValueError('background is not a JPEG')
    pos = 2
    while pos + 4 <= len(data):
        if data[pos] != 0xFF:
            pos += 1
            continue
        while pos < len(data) and data[pos] == 0xFF:
            pos += 1
        if pos >= len(data):
            break
        marker = data[pos]
        pos += 1
        if marker in (0xD8, 0xD9) or 0xD0 <= marker <= 0xD7:
            continue
        if pos + 2 > len(data):
            break
        size = struct.unpack_from('>H', data, pos)[0]
        if size < 2 or pos + size > len(data):
            break
        if marker in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                      0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            height, width = struct.unpack_from('>HH', data, pos + 3)
            return width, height
        pos += size
    raise ValueError('could not read JPEG dimensions')


def define_jpeg(character, jpeg):
    return tag(21, struct.pack('<H', character) + jpeg)  # DefineBitsJPEG2


def straight_edge(writer, dx, dy):
    count = max(2, signed_bits(dx, dy))
    if count > 17:
        raise ValueError('shape edge is too large for DefineShape')
    writer.u(1, 1)  # edge record
    writer.u(1, 1)  # straight
    writer.u(count - 2, 4)
    writer.u(1, 1)  # general line
    writer.s(dx, count)
    writer.s(dy, count)


def define_bitmap_shape(character, bitmap, width=1920, height=1080):
    twips = 20
    payload = bytearray(struct.pack('<H', character))
    payload.extend(rect(0, width * twips, 0, height * twips))
    # One clipped bitmap fill. A 20.0 bitmap matrix maps one bitmap pixel to 20 twips.
    payload.extend(bytes((1, 0x41)))
    payload.extend(struct.pack('<H', bitmap))
    payload.extend(matrix(20.0, 20.0))
    payload.append(0)  # no line styles

    writer = BitWriter()
    writer.u(1, 4)  # NumFillBits
    writer.u(0, 4)  # NumLineBits
    # StyleChangeRecord: move to 0,0 and select FillStyle1=1.
    writer.u(0, 1)
    writer.u(0, 1)  # new styles
    writer.u(0, 1)  # line style
    writer.u(1, 1)  # fill style 1
    writer.u(0, 1)  # fill style 0
    writer.u(1, 1)  # move
    writer.u(1, 5)
    writer.s(0, 1)
    writer.s(0, 1)
    writer.u(1, 1)
    straight_edge(writer, width * twips, 0)
    straight_edge(writer, 0, height * twips)
    straight_edge(writer, -width * twips, 0)
    straight_edge(writer, 0, -height * twips)
    writer.u(0, 1)  # EndShapeRecord type
    writer.u(0, 5)
    payload.extend(writer.finish())
    return tag(2, bytes(payload))


def place_object(character, depth=1, move=False):
    flags = 0x06 | (0x01 if move else 0)  # HasCharacter | HasMatrix | Move
    payload = bytes((flags,)) + struct.pack('<HH', depth, character) + matrix()
    return tag(26, payload)


def selector_sprite(character, shape_ids):
    payload = bytearray(struct.pack('<HH', character, len(shape_ids)))
    for index, shape in enumerate(shape_ids):
        payload.extend(place_object(shape, 1, move=index != 0))
        payload.extend(tag(1))  # ShowFrame
    payload.extend(tag(0))
    return tag(39, bytes(payload))


def u30(value):
    value &= 0x3FFFFFFF
    result = bytearray()
    while True:
        byte = value & 0x7F
        value >>= 7
        if value:
            byte |= 0x80
        result.append(byte)
        if not value:
            return bytes(result)


def abc_qname(namespace, name):
    return bytes((0x07,)) + u30(namespace) + u30(name)


def abc_method():
    # param_count=0, return_type=*, name=anonymous, flags=0
    return b'\x00\x00\x00\x00'


def abc_method_body(method, code, max_stack, local_count, init_scope, max_scope):
    return (u30(method) + u30(max_stack) + u30(local_count) + u30(init_scope) +
            u30(max_scope) + u30(len(code)) + code + b'\x00\x00')


def abc_op_u30(opcode, value):
    return bytes((opcode,)) + u30(value)


def abc_op_u30u30(opcode, first, second):
    return bytes((opcode,)) + u30(first) + u30(second)


def random_selector_abc_tag():
    """Build a minimal AS3 class that selects selector frame 1..6 in its constructor.

    The scope-chain setup intentionally mirrors the Animate-generated class already present in
    Bloodborne's classic nowloading.gfx. That keeps the helper compatible with Scaleform's AVM2
    verifier while avoiding any build-time dependency on Flex/JPEXS.
    """
    strings = [
        CLASS_NAME,
        'flash.display', 'MovieClip', 'Object',
        'flash.events', 'EventDispatcher',
        'DisplayObject', 'InteractiveObject', 'DisplayObjectContainer', 'Sprite',
        'Math', 'random', 'gotoAndStop',
    ]
    namespaces = [(0x16, 0), (0x16, 2), (0x16, 5)]  # public, flash.display, flash.events
    multinames = [
        (1, 1),   # CLASS_NAME
        (2, 3),   # flash.display::MovieClip
        (1, 4),   # Object
        (3, 6),   # flash.events::EventDispatcher
        (2, 7),   # DisplayObject
        (2, 8),   # InteractiveObject
        (2, 9),   # DisplayObjectContainer
        (2, 10),  # Sprite
        (1, 11),  # Math
        (1, 12),  # random
        (1, 13),  # gotoAndStop
    ]

    abc = bytearray(struct.pack('<HH', 16, 46))
    abc.extend(b'\x00\x00\x00')  # int/uint/double pools
    abc.extend(u30(len(strings) + 1))
    for value in strings:
        encoded = value.encode('utf-8')
        abc.extend(u30(len(encoded)))
        abc.extend(encoded)
    abc.extend(u30(len(namespaces) + 1))
    for kind, name in namespaces:
        abc.extend(bytes((kind,)))
        abc.extend(u30(name))
    abc.extend(b'\x00')  # namespace-set pool
    abc.extend(u30(len(multinames) + 1))
    for namespace, name in multinames:
        abc.extend(abc_qname(namespace, name))

    # Methods: class static init, instance constructor, script init.
    abc.extend(u30(3))
    abc.extend(abc_method() * 3)
    abc.extend(b'\x00')  # metadata

    # One sealed class: BBRandomLoadingBackground extends flash.display.MovieClip.
    abc.extend(u30(1))
    abc.extend(u30(1) + u30(2) + b'\x01' + b'\x00' + u30(1) + b'\x00')
    abc.extend(u30(0) + b'\x00')  # class cinit + no static traits

    # Script exposes the class through a Class trait.
    class_trait = u30(1) + b'\x04' + u30(1) + u30(0)
    abc.extend(u30(1) + u30(2) + u30(1) + class_trait)

    cinit = bytes((0xD0, 0x30, 0x47))
    # this.gotoAndStop(1 + int(Math.random() * EXPECTED_COUNT))
    iinit = bytearray((0xD0, 0x30, 0xD0, 0x49, 0x00, 0xD0))
    iinit.extend(abc_op_u30(0x60, 9))             # getlex Math
    iinit.extend(abc_op_u30u30(0x46, 10, 0))     # callproperty random, 0
    iinit.extend(bytes((0x24, EXPECTED_COUNT, 0xA2, 0x73, 0x24, 0x01, 0xC5)))
    iinit.extend(abc_op_u30u30(0x4F, 11, 1))     # callpropvoid gotoAndStop, 1
    iinit.append(0x47)

    # Animate's class initializer keeps the superclass chain in the captured scope stack.
    script = bytearray((0xD0, 0x30, 0x65, 0x00))
    for multiname in (3, 4, 5, 6, 7, 8, 2):
        script.extend(abc_op_u30(0x60, multiname))
        script.append(0x30)
    script.extend(abc_op_u30(0x60, 2))
    script.extend(abc_op_u30(0x58, 0))
    script.extend(b'\x1D' * 7)
    script.extend(abc_op_u30(0x68, 1))
    script.append(0x47)

    abc.extend(u30(3))
    abc.extend(abc_method_body(0, cinit, 1, 1, 9, 10))
    abc.extend(abc_method_body(1, bytes(iinit), 3, 1, 10, 11))
    abc.extend(abc_method_body(2, bytes(script), 2, 1, 1, 9))

    payload = struct.pack('<I', 0) + CLASS_NAME.encode('utf-8') + b'\0' + bytes(abc)
    return tag(82, payload)


def patch_main_sprite(payload, new_background):
    sprite_id, frame_count = struct.unpack_from('<HH', payload, 0)
    output = bytearray(payload[:4])
    pos = 4
    frame = 1
    replaced = False
    labels = set()
    while pos < len(payload):
        code, child, _, end = parse_tag(payload, pos)
        pos = end
        if code == 43:
            labels.add(child.split(b'\0', 1)[0].decode('utf-8', errors='replace'))
        if code == 26 and not replaced and frame == 1 and len(child) >= 5:
            flags = child[0]
            depth = struct.unpack_from('<H', child, 1)[0]
            if depth == 2 and flags & 0x02:
                modified = bytearray(child)
                struct.pack_into('<H', modified, 3, new_background)
                child = bytes(modified)
                replaced = True
        output.extend(tag(code, child))
        if code == 1:
            frame += 1
        if code == 0:
            break
    required = {'FadeIn', 'Normal', 'FadeOut'}
    if not replaced:
        raise ValueError(f'could not locate the classic background placement in sprite {sprite_id}')
    if frame_count != 120 or not required.issubset(labels):
        raise ValueError(f'unexpected classic timeline: frames={frame_count}, labels={sorted(labels)}')
    return bytes(output)


def build_variants(source, images, outputs):
    """Generate six conservative GFX variants without injecting AVM2.

    Each variant keeps Bloodborne's original classic movie/timeline/bytecode intact and only
    redirects the existing background placement to one new static bitmap sprite. Random selection
    happens in the host file redirect, before Scaleform parses the movie.
    """
    original = source.read_bytes()
    if original[:3] != b'GFX':
        raise ValueError('source nowloading.gfx is not an uncompressed Scaleform GFX')
    if struct.unpack_from('<I', original, 4)[0] != len(original):
        raise ValueError('source GFX length header does not match file size')

    image_data = [path.read_bytes() for path in images]
    for path, data in zip(images, image_data):
        if jpeg_dimensions(data) != (1920, 1080):
            raise ValueError(f'{path.name}: expected 1920x1080 JPEG')

    start = header_end(original)
    pos = start
    main_sprite = None
    max_character = 0
    top = []
    while pos < len(original):
        code, payload, _, end = parse_tag(original, pos)
        pos = end
        if code in (2, 22, 32, 39, 83, 21, 35, 36, 87, 90) and len(payload) >= 2:
            max_character = max(max_character, struct.unpack_from('<H', payload, 0)[0])
        if code == 76:
            for character, name in symbol_classes(payload):
                max_character = max(max_character, character)
                if name == 'NowLoading_fla.NowLoading_1':
                    main_sprite = character
        top.append((code, payload))
        if code == 0:
            break
    if main_sprite is None:
        raise ValueError('classic NowLoading_fla.NowLoading_1 SymbolClass was not found')

    built = []
    for image_index, (jpeg, output) in enumerate(zip(image_data, outputs), start=1):
        # Reusing only standard SWF/GFX display tags is intentionally conservative. The previous
        # experiment injected a generated AVM2 class and crashed inside the guest as soon as
        # Scaleform opened the movie.
        bitmap_id = max_character + 1
        shape_id = max_character + 2
        sprite_id = max_character + 3
        additions = (
            define_jpeg(bitmap_id, jpeg) +
            define_bitmap_shape(shape_id, bitmap_id) +
            selector_sprite(sprite_id, [shape_id])
        )

        rebuilt = bytearray(original[:start])
        inserted_assets = patched_main = False
        for code, payload in top:
            if code == 39 and len(payload) >= 2 and struct.unpack_from('<H', payload, 0)[0] == main_sprite:
                if not inserted_assets:
                    rebuilt.extend(additions)
                    inserted_assets = True
                payload = patch_main_sprite(payload, sprite_id)
                patched_main = True
            rebuilt.extend(tag(code, payload))

        if not inserted_assets or not patched_main:
            raise ValueError(f'custom loading GFX variant {image_index} did not patch the main sprite')
        rebuilt[:3] = b'GFX'
        struct.pack_into('<I', rebuilt, 4, len(rebuilt))
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(rebuilt)
        built.append(len(rebuilt))
    return built


def fingerprint(paths):
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='the user-owned dvdroot_ps4/menu/nowloading.gfx')
    parser.add_argument('--images-dir', required=True, type=Path)
    parser.add_argument('--out-dir', required=True, type=Path)
    args = parser.parse_args()

    images = sorted([*args.images_dir.glob('loading_*.jpg'), *args.images_dir.glob('loading_*.jpeg')])
    if len(images) != EXPECTED_COUNT:
        raise SystemExit(f'Expected {EXPECTED_COUNT} packaged loading backgrounds, found {len(images)}')
    required = [args.source, *images]
    for path in required:
        if not path.is_file():
            raise SystemExit(f'Missing loading-screen input: {path}')

    outputs = [args.out_dir / f'nowloading-custom-{index:02d}.gfx'
               for index in range(1, EXPECTED_COUNT + 1)]
    # Include the builder itself so a code fix cannot accidentally reuse a stale generated movie.
    key = fingerprint([Path(__file__), *required])
    stamp = args.out_dir / 'loading-screens.sha256'
    if all(path.is_file() for path in outputs) and stamp.is_file() and stamp.read_text().strip() == key:
        print(f'Loading screens: cached {EXPECTED_COUNT} safe classic GFX variants')
        return
    try:
        sizes = build_variants(args.source, images, outputs)
    except (OSError, ValueError, struct.error) as error:
        raise SystemExit(f'Loading screens: generation failed: {error}')
    args.out_dir.mkdir(parents=True, exist_ok=True)
    stamp.write_text(key + '\n')
    print(f'Loading screens: generated {EXPECTED_COUNT} safe classic GFX variants '
          f'({min(sizes)}..{max(sizes)} bytes, original game file unchanged)')


if __name__ == '__main__':
    main()
