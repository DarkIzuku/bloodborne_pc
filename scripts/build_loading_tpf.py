#!/usr/bin/env python3
"""Build one extended Bloodborne classic-loading TPF containing all six packaged artworks.

The user's dvdroot_ps4/menu/nowloading.tpf.dcx is never modified. Its original texture records and
payloads are preserved, then six BC/DXGI-compatible PS4 textures named BB_Loading_01..06 are
appended. The six conservative GFX variants can therefore choose a different texture every loading
screen while the game resource manager may cache this TPF for the whole process.
"""
import argparse
import hashlib
import math
from pathlib import Path
import struct
import subprocess
import tempfile
import zlib

EXPECTED_COUNT = 6
TARGET_STEM = "MENU_NowLoading_00001"

DXGI = {
    28: ("R8G8B8A8_UNORM", 32, 1, 4),
    29: ("R8G8B8A8_UNORM_SRGB", 32, 1, 4),
    65: ("A8_UNORM", 8, 1, 1),
    71: ("BC1_UNORM", 4, 4, 8),
    72: ("BC1_UNORM_SRGB", 4, 4, 8),
    74: ("BC2_UNORM", 8, 4, 16),
    75: ("BC2_UNORM_SRGB", 8, 4, 16),
    77: ("BC3_UNORM", 8, 4, 16),
    78: ("BC3_UNORM_SRGB", 8, 4, 16),
    80: ("BC4_UNORM", 4, 4, 8),
    81: ("BC4_SNORM", 4, 4, 8),
    83: ("BC5_UNORM", 8, 4, 16),
    84: ("BC5_SNORM", 8, 4, 16),
    87: ("B8G8R8A8_UNORM", 32, 1, 4),
    91: ("B8G8R8A8_UNORM_SRGB", 32, 1, 4),
    95: ("BC6H_UF16", 8, 4, 16),
    96: ("BC6H_SF16", 8, 4, 16),
    98: ("BC7_UNORM", 8, 4, 16),
    99: ("BC7_UNORM_SRGB", 8, 4, 16),
}


def decompress_dcx(data):
    if data[:4] != b"DCX\0" or len(data) < 76:
        raise ValueError("nowloading.tpf.dcx is not a supported DCX container")
    if data[40:44] != b"DFLT":
        raise ValueError(f"unsupported loading TPF DCX compression {data[40:44]!r}")
    decompressed_size = struct.unpack_from(">I", data, 28)[0]
    compressed_size = struct.unpack_from(">I", data, 32)[0]
    if data[68:72] != b"DCA\0":
        raise ValueError("unexpected DCX DCA header")
    payload = zlib.decompress(data[76:76 + compressed_size])
    if len(payload) != decompressed_size:
        raise ValueError("DCX decompressed size mismatch")
    return payload, data[:76]


def compress_dcx(payload, header):
    compressed = zlib.compress(payload, level=7)
    out = bytearray(header)
    struct.pack_into(">I", out, 28, len(payload))
    struct.pack_into(">I", out, 32, len(compressed))
    return bytes(out) + compressed


def read_c_string(data, offset, encoding_type):
    if offset < 0 or offset >= len(data):
        raise ValueError("TPF texture name offset is outside the file")
    if encoding_type == 1:
        end = offset
        while end + 1 < len(data):
            if data[end:end + 2] == b"\0\0":
                return data[offset:end].decode("utf-16-le", errors="replace")
            end += 2
        raise ValueError("unterminated UTF-16 TPF texture name")
    end = data.find(b"\0", offset)
    if end < 0:
        raise ValueError("unterminated TPF texture name")
    return data[offset:end].decode("shift_jis", errors="replace")


def encode_c_string(value, encoding_type):
    if encoding_type == 1:
        return value.encode("utf-16-le") + b"\0\0"
    return value.encode("shift_jis") + b"\0"


def parse_tpf(tpf):
    if len(tpf) < 16 or tpf[:4] != b"TPF\0":
        raise ValueError("decompressed loading texture package is not a TPF")
    file_count = struct.unpack_from("<I", tpf, 8)[0]
    platform, tpf_flags, encoding_type = tpf[12], tpf[13], tpf[14]
    if platform != 4:
        raise ValueError(f"expected PS4 TPF platform 4, found {platform}")
    records = []
    pos = 16
    for index in range(file_count):
        if pos + 36 > len(tpf):
            raise ValueError(f"truncated PS4 TPF texture record {index}")
        record_offset = pos
        data_offset, data_size = struct.unpack_from("<Ii", tpf, pos)
        fmt, texture_type, mip_count, texture_flags = tpf[pos + 8:pos + 12]
        width, height = struct.unpack_from("<hh", tpf, pos + 12)
        texture_count, unk2, stem_offset, has_float, dxgi = struct.unpack_from("<iiIii", tpf, pos + 16)
        pos += 36
        if has_float:
            if pos + 8 > len(tpf):
                raise ValueError(f"truncated TPF float metadata for texture {index}")
            _, float_size = struct.unpack_from("<ii", tpf, pos)
            pos += 8
            if float_size < 0 or pos + float_size > len(tpf):
                raise ValueError(f"invalid TPF float metadata size for texture {index}")
            pos += float_size
        stem = read_c_string(tpf, stem_offset, encoding_type)
        if data_offset < 0 or data_size < 0 or data_offset + data_size > len(tpf):
            raise ValueError(f"invalid TPF data range for texture {stem!r}")
        records.append({
            "index": index,
            "meta": bytes(tpf[record_offset:pos]),
            "data_offset": data_offset,
            "data_size": data_size,
            "format": fmt,
            "texture_type": texture_type,
            "mip_count": mip_count,
            "texture_flags": texture_flags,
            "width": width,
            "height": height,
            "texture_count": texture_count,
            "unk2": unk2,
            "stem_offset": stem_offset,
            "dxgi": dxgi,
            "stem": stem,
            "stored": bytes(tpf[data_offset:data_offset + data_size]),
        })
    if not records:
        raise ValueError("loading TPF contains no textures")
    return records, tpf_flags, encoding_type


def dds_payload(path):
    data = path.read_bytes()
    if len(data) < 128 or data[:4] != b"DDS ":
        raise ValueError("texconv did not produce a DDS")
    offset = 148 if data[84:88] == b"DX10" else 128
    if len(data) < offset:
        raise ValueError("truncated DDS")
    return data[offset:]


def morton(index, width, height):
    params = [1, 1, index, width, height, 0, 0]
    while params[3] > 1 or params[4] > 1:
        if params[3] > 1:
            params[5] += params[1] * (params[2] & 1)
            params[1] *= 2
            params[2] >>= 1
            params[3] >>= 1
        if params[4] > 1:
            params[6] += params[0] * (params[2] & 1)
            params[0] *= 2
            params[2] >>= 1
            params[4] >>= 1
    return params[6] * width + params[5]


def swizzle_level(linear, width, height, block_dim, bytes_per_set):
    sx = max(1, (width + block_dim - 1) // block_dim)
    sy = max(1, (height + block_dim - 1) // block_dim)
    expected = sx * sy * bytes_per_set
    if len(linear) < expected:
        raise ValueError(f"DDS mip data too short: {len(linear)} < {expected}")
    out = bytearray(expected)
    pos = 0
    for tile_y in range((sy + 7) // 8):
        for tile_x in range((sx + 7) // 8):
            for source_tile in range(64):
                if pos + bytes_per_set > len(out):
                    return bytes(out)
                tiled = morton(source_tile, 8, 8)
                row, col = tiled // 8, tiled % 8
                x, y = tile_x * 8 + col, tile_y * 8 + row
                if x < sx and y < sy:
                    src = (y * sx + x) * bytes_per_set
                    out[pos:pos + bytes_per_set] = linear[src:src + bytes_per_set]
                pos += bytes_per_set
    return bytes(out)


def swizzle_dds(payload, width, height, mip_count, dxgi):
    try:
        _, _, block_dim, bytes_per_set = DXGI[dxgi]
    except KeyError:
        raise ValueError(f"unsupported PS4 loading texture DXGI format {dxgi}")
    levels = mip_count or (int(math.floor(math.log2(max(width, height)))) + 1)
    out = bytearray()
    pos = 0
    w, h = width, height
    for level in range(levels):
        sx = max(1, (w + block_dim - 1) // block_dim)
        sy = max(1, (h + block_dim - 1) // block_dim)
        size = sx * sy * bytes_per_set
        if pos + size > len(payload):
            raise ValueError(f"DDS ended before mip {level} ({pos + size} > {len(payload)})")
        out.extend(swizzle_level(payload[pos:pos + size], w, h, block_dim, bytes_per_set))
        pos += size
        w, h = max(1, w // 2), max(1, h // 2)
    return bytes(out)


def make_texture(image, target, texconv, work):
    if target["dxgi"] not in DXGI:
        raise ValueError(f"unsupported target DXGI {target['dxgi']}")
    format_name = DXGI[target["dxgi"]][0]
    mip_arg = str(target["mip_count"] if target["mip_count"] else 0)
    command = [
        str(texconv), "-y", "-nologo", "-f", format_name,
        "-w", str(target["width"]), "-h", str(target["height"]),
        "-m", mip_arg, "-o", str(work), str(image),
    ]
    result = subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
    if result.returncode:
        raise ValueError(f"texconv failed for {image.name}: {result.stdout.strip()}")
    candidates = list(work.glob(image.stem + ".DDS")) + list(work.glob(image.stem + ".dds"))
    if not candidates:
        candidates = list(work.glob("*.DDS")) + list(work.glob("*.dds"))
    if not candidates:
        raise ValueError(f"texconv produced no DDS for {image.name}")
    payload = dds_payload(candidates[0])
    logical = swizzle_dds(payload, target["width"], target["height"],
                          target["mip_count"], target["dxgi"])
    return zlib.compress(logical, level=7) if target["texture_flags"] in (2, 3) else logical


def build_extended_tpf(original, records, target, custom_stored, encoding_type):
    entries = []
    for record in records:
        entries.append({"meta": record["meta"], "stem": record["stem"], "stored": record["stored"]})
    for index, stored in enumerate(custom_stored, start=1):
        entries.append({
            "meta": target["meta"],
            "stem": f"BB_Loading_{index:02d}",
            "stored": stored,
        })

    out = bytearray(original[:16])
    record_positions = []
    for entry in entries:
        record_positions.append(len(out))
        out.extend(entry["meta"])

    for position, entry in zip(record_positions, entries):
        stem_offset = len(out)
        struct.pack_into("<I", out, position + 24, stem_offset)
        out.extend(encode_c_string(entry["stem"], encoding_type))

    while len(out) & 3:
        out.append(0)
    data_start = len(out)

    for position, entry in zip(record_positions, entries):
        while len(out) & 3:
            out.append(0)
        data_offset = len(out)
        out.extend(entry["stored"])
        struct.pack_into("<I", out, position, data_offset)
        struct.pack_into("<i", out, position + 4, len(entry["stored"]))

    struct.pack_into("<i", out, 4, len(out) - data_start)
    struct.pack_into("<I", out, 8, len(entries))
    return bytes(out)


def fingerprint(paths):
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.name.encode())
        digest.update(path.read_bytes())
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, help="user-owned dvdroot_ps4/menu/nowloading.tpf.dcx")
    parser.add_argument("--images-dir", required=True, type=Path)
    parser.add_argument("--texconv", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    images = sorted([*args.images_dir.glob("loading_*.jpg"), *args.images_dir.glob("loading_*.jpeg")])
    if len(images) != EXPECTED_COUNT:
        raise SystemExit(f"Expected {EXPECTED_COUNT} loading backgrounds, found {len(images)}")
    if not args.source.is_file():
        raise SystemExit(f"Missing classic loading texture package: {args.source}")
    if not args.texconv.is_file():
        raise SystemExit(f"Missing packaged texconv: {args.texconv}")

    key = fingerprint([Path(__file__), args.source, *images])
    stamp = args.output.with_suffix(args.output.suffix + ".sha256")
    if args.output.is_file() and stamp.is_file() and stamp.read_text().strip() == key:
        print("Loading screens: cached extended classic TPF with 6 custom textures")
        return

    try:
        source = args.source.read_bytes()
        tpf, dcx_header = decompress_dcx(source)
        records, _, encoding_type = parse_tpf(tpf)
        targets = [r for r in records if r["stem"].lower() == TARGET_STEM.lower()]
        if not targets:
            names = ", ".join(r["stem"] for r in records)
            raise ValueError(f"{TARGET_STEM} not found in loading TPF; textures: {names}")
        target = targets[0]
        if target["texture_type"] != 0:
            raise ValueError(f"loading texture is not a 2D texture (type={target['texture_type']})")
        if target["dxgi"] not in DXGI:
            raise ValueError(f"unsupported loading texture DXGI {target['dxgi']}")
        print(
            "Loading screens: TPF target "
            f"{target['stem']} {target['width']}x{target['height']} "
            f"format={target['format']} dxgi={target['dxgi']} "
            f"mips={target['mip_count']} flags={target['texture_flags']} "
            f"stored={target['data_size']} bytes; original records={len(records)}"
        )
        custom = []
        with tempfile.TemporaryDirectory(prefix="bb-loading-") as tmp:
            tmp = Path(tmp)
            for index, image in enumerate(images, start=1):
                work = tmp / f"{index:02d}"
                work.mkdir()
                custom.append(make_texture(image, target, args.texconv, work))
        rebuilt = build_extended_tpf(tpf, records, target, custom, encoding_type)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_bytes(compress_dcx(rebuilt, dcx_header))
    except (OSError, ValueError, struct.error, zlib.error) as error:
        raise SystemExit(f"Loading screens: TPF generation failed: {error}")

    stamp.write_text(key + "\n")
    print("Loading screens: built one extended classic TPF with original resources + 6 custom textures; "
          "original game files unchanged")


if __name__ == "__main__":
    main()
