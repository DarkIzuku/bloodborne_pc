# SPDX-License-Identifier: GPL-3.0-or-later
"""Local, version-gated asset preparation for engine features ported from bbhost.

Adapted from droogie/bbhost engine/dream_mirror_layout.cpp and gcn/container.cpp,
upstream 7c790536c2c27ad7bb5115e3b1a12d7ffd7a972e. Game files stay local.
"""
import hashlib
import os
import json
import subprocess
from pathlib import Path
import struct
import zlib

LAYOUT = Path('dvdroot_ps4/map/mapstudio/m21_00_00_00.msb.dcx')
INPUT_SHA = '8e1de0ee223cd5137a93383cfc7bb1d27964bc480c63e35013674717b403ef8f'
OUTPUT_SHA = '99d5c7b7f52307344c6e8296b499d5e57969f21c443fcc607cb42cb2c38c8afa'
EBOOT_SHA = {'941f887a562aae054fac35af8cc8f27cf075f3d4cc2e029fb5ae2a663aaa5ae7',
             'cec1b276e7f9e4db978e57f524f41fbaac594530a3437b002e23f3fab14b4f86'}
IMAGE_SHA = '071df19c8880086d97182dbc057bc8cb37badaca57d9112683836b24a0444c0a'
HOOKS = ((0x1c1cce0, bytes.fromhex('554889e54157415653504d89ce4889fb')),
         (0x2034770, bytes.fromhex('554889e54157415641554154534883ec38')))


def sha(data):
    return hashlib.sha256(data).hexdigest()


def dcx_unpack(data):
    if data[:4] != b'DCX\0':
        return data
    if len(data) < 0x4c or data[0x28:0x2c] != b'DFLT':
        raise ValueError('Unsupported DCX format')
    dca = data.find(b'DCA\0')
    if dca < 0 or dca + 8 > len(data):
        raise ValueError('DCX without a valid DCA header')
    skip = struct.unpack_from('>I', data, dca + 4)[0]
    if skip < 8 or dca + skip >= len(data):
        raise ValueError('Invalid DCX compressed offset')
    expected = struct.unpack_from('>I', data, 0x1c)[0]
    if not 0 < expected <= 32 * 1024 * 1024:
        raise ValueError('Invalid DCX uncompressed size')
    stream = zlib.decompressobj()
    raw = stream.decompress(data[dca + skip:], expected + 1)
    if len(raw) != expected or not stream.eof or stream.unused_data:
        raise ValueError('Invalid DCX compressed data or size')
    return raw


def dcx_pack(raw):
    # Same DFLT header/level as bbhost; sizes at DCS +4 / +8 are big-endian.
    header = bytearray.fromhex(
        '44435800000100000000001800000024000000440000004c'
        '4443530000000000000000004443500044464c5400000020'
        '09000000000000000000000000000000000001014443410000000008')
    packed = zlib.compress(raw, 9)
    struct.pack_into('>II', header, 0x1c, len(raw), len(packed))
    return bytes(header) + packed


def mirror_layout(raw):
    if sha(raw) != INPUT_SHA:
        raise ValueError('Dream mirror requires the original 1.09 Dream layout')
    b = bytearray(raw)
    lists = []
    at = 0x10
    for index in range(4):
        if not 0 <= at <= len(b) - 16:
            raise ValueError('Invalid MSB list header')
        count = struct.unpack_from('<i', b, at + 4)[0]
        if not 1 <= count <= (len(b) - at - 16) // 8:
            raise ValueError('Invalid MSB list size')
        offsets = struct.unpack_from(f'<{count}q', b, at + 16)
        if any(not 0 < value < len(b) for value in offsets[:-1]):
            raise ValueError('Invalid MSB entry')
        lists.append((at + 16, offsets[:-1]))
        at = offsets[-1]
        if index < 3 and not 0 < at < len(b):
            raise ValueError('Invalid next MSB list')
    events, (table, parts) = lists[1][1], lists[3]
    source, target, first_enemy = 807, 742, 736
    if len(parts) <= source + 1:
        raise ValueError('Dream layout contains too few parts')
    starts = sorted(offset for _, entries in lists for offset in entries)

    def remap(slot):
        return target if slot == source else slot + 1 if target <= slot < source else slot

    # Every part reference must follow the moved entry, including the moved part itself.
    for entries, fields in ((events, (0x48, 0x50, 0x5c)),
                            (parts, (0x128, 0x13c, 0x144, 0x178, 0x180, 0x188))):
        for entry in entries:
            end = next((x for x in starts if x > entry), len(b))
            for field in fields:
                if entry + field + 4 > end:
                    continue
                slot = struct.unpack_from('<i', b, entry + field)[0]
                if slot >= 0 and remap(slot) != slot:
                    struct.pack_into('<i', b, entry + field, remap(slot))
    for i, entry in enumerate(parts):
        kind, number = struct.unpack_from('<ii', b, entry + 0x14)
        if kind == 2 and i >= target:
            struct.pack_into('<i', b, entry + 0x18, number + 1)
        if kind == 10 and i > source:
            struct.pack_into('<i', b, entry + 0x18, number - 1)
    moved = parts[source]
    if struct.unpack_from('<i', b, moved + 0x14)[0] != 10:
        raise ValueError('Mirror stand-in is not a DummyEnemy')
    struct.pack_into('<ii', b, moved + 0x14, 2, target - first_enemy)
    start, old, end = parts[target], parts[source], parts[source + 1]
    if not start < old < end:
        raise ValueError('Dream parts are not ordered')
    b[start:end] = b[old:end] + b[start:old]
    size = end - old
    for i in range(target, source):
        struct.pack_into('<q', b, table + 8 * (i + 1), parts[i] + size)
    struct.pack_into('<q', b, table + 8 * target, start)
    if sha(b) != OUTPUT_SHA:
        raise ValueError('Mirror layout does not match the verified bbhost result')
    return bytes(b)


def verify_executable(elf_path):
    # The two full-file identities have identical loadable 1.09 code. A version string
    # alone, or BB_SKIP_GAME_CHECK, never enables address-based hooks.
    elf = Path(elf_path).read_bytes()
    if sha(elf) not in EBOOT_SHA:
        raise ValueError('Mirror hooks require the verified 1.09 executable')
    phoff = struct.unpack_from('<Q', elf, 32)[0]
    entsize, count = struct.unpack_from('<HH', elf, 54)
    if entsize != 56 or not 0 < count < 256:
        raise ValueError('Invalid executable program headers')
    segments = [struct.unpack_from('<IIQQQQQQ', elf, phoff + i * 56) for i in range(count)]
    for address, expected in HOOKS:
        found = False
        for kind, flags, offset, va, _, size, _, _ in segments:
            if kind == 1 and flags & 1 and va <= address and address + len(expected) <= va + size:
                found = elf[offset + address - va:offset + address - va + len(expected)] == expected
                break
        if not found:
            raise ValueError(f'Unexpected 1.09 engine bytes at {address:#x}')
    return IMAGE_SHA


def prepare_mirror(game, elf_path, cache):
    verify_executable(elf_path)
    raw = dcx_unpack((Path(game) / LAYOUT).read_bytes())
    rewritten = mirror_layout(raw)
    destination = Path(cache) / 'dream-mirror' / LAYOUT
    # Verify cached output as well; no stale marker bypasses either input check.
    try:
        if sha(dcx_unpack(destination.read_bytes())) == OUTPUT_SHA:
            return destination.resolve()
    except (OSError, ValueError, zlib.error):
        pass
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_suffix(destination.suffix + '.tmp')
    temporary.write_bytes(dcx_pack(rewritten))
    os.replace(temporary, destination)
    return destination.resolve()


def prepare_menu_assets(game, elf_path, cache, tool):
    """Generate bbhost's native option movie and messages from the player's own dump.

    Validate every language, source and cached output together before publishing the
    directory to the runtime. No version string or stale stamp enables native hooks.
    """
    verify_executable(elf_path)
    game, tool = Path(game), Path(tool)
    root = Path(cache) / 'pc-menus'
    movie = Path('dvdroot_ps4/menu/optionsetting.gfx')
    sources = [(movie, 'movie')]
    message_dir = game / 'dvdroot_ps4/msg'
    sources += [(p.relative_to(game), 'messages')
                for p in sorted(message_dir.glob('*/menu.msgbnd.dcx')) if p.is_file()]
    if len(sources) < 2 or len(sources) > 24:
        raise ValueError('Native options require valid menu bundles for every installed language')
    generator = sha(tool.read_bytes())
    manifest_path = root / 'manifest.json'
    try:
        previous = json.loads(manifest_path.read_text(encoding='utf-8'))
    except (OSError, ValueError):
        previous = {}
    rows = {}
    for relative, kind in sources:
        source, destination = game / relative, root / relative
        if source.stat().st_size > 64 * 1024 * 1024:
            raise ValueError(f'Native menu source is oversized: {relative}')
        digest = sha(source.read_bytes())
        prior = previous.get('files', {}).get(relative.as_posix(), {})
        current = None
        if previous.get('generator') == generator and prior.get('input') == digest:
            try:
                current = sha(destination.read_bytes())
                if current != prior.get('output'):
                    current = None
            except OSError:
                pass
        if current is None:
            destination.parent.mkdir(parents=True, exist_ok=True)
            temporary = destination.with_name(destination.name + f'.{os.getpid()}.tmp')
            try:
                result = subprocess.run([str(tool), kind, str(source), str(temporary)],
                                        capture_output=True, text=True, errors='replace')
                if result.returncode:
                    raise ValueError(result.stderr.strip() or f'Menu generator failed: {relative}')
                current = sha(temporary.read_bytes())
                os.replace(temporary, destination)
            finally:
                temporary.unlink(missing_ok=True)
        rows[relative.as_posix()] = {'input': digest, 'output': current}
    root.mkdir(parents=True, exist_ok=True)
    temporary = manifest_path.with_suffix(f'.{os.getpid()}.tmp')
    temporary.write_text(json.dumps({'generator': generator, 'files': rows}, indent=2), encoding='utf-8')
    os.replace(temporary, manifest_path)
    return root.resolve()
