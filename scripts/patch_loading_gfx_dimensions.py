#!/usr/bin/env python3
"""Patch only the logical dimensions of Bloodborne's classic external loading artwork.

The original GFX remains structurally identical: same tags, same lengths, same character IDs,
same file name, same timeline and ActionScript. Only DefineExternalImage2.targetWidth/targetHeight
for MENU_NowLoading_00001.tga are changed from 2048x1024 to 1920x1080.

Why: the game displays a 1920x1080 classic loading rectangle but the external image advertises
2048x1024. With a full-screen custom picture in the original TPF slot, the last 56 logical rows
wrap/repeat. Advertising the intended UI size makes Scaleform rescale the existing external image
instead of sampling past the 1024-row texture.
"""
import argparse
from pathlib import Path
import struct

TARGET = "MENU_NowLoading_00001.tga"
TARGET_WIDTH = 1920
TARGET_HEIGHT = 1080


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
    record = struct.unpack_from("<H", data, pos)[0]
    pos += 2
    code, length = record >> 6, record & 0x3F
    if length == 0x3F:
        length = struct.unpack_from("<I", data, pos)[0]
        pos += 4
    payload_start = pos
    end = pos + length
    if end > len(data):
        raise ValueError("truncated GFX tag")
    return code, payload_start, end


def read_net_string(data, pos, end):
    if pos >= end:
        raise ValueError("truncated GFX net string")
    length = data[pos]
    pos += 1
    if pos + length > end:
        raise ValueError("truncated GFX net string data")
    return data[pos:pos + length].decode("utf-8", errors="replace"), pos + length


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("--out", required=True, type=Path)
    args = p.parse_args()

    data = args.source.read_bytes()
    if data[:3] != b"GFX":
        raise SystemExit("Loading screens: classic nowloading.gfx is not an uncompressed GFX")
    if struct.unpack_from("<I", data, 4)[0] != len(data):
        raise SystemExit("Loading screens: classic GFX length header does not match file size")

    out = bytearray(data)
    pos = header_end(data)
    patched = []
    while pos < len(data):
        code, payload_start, end = parse_tag(data, pos)
        if code == 1009 and end - payload_start >= 10:  # DefineExternalImage2
            image_id, id_type, bitmap_format, width, height = struct.unpack_from(
                "<HHHHH", data, payload_start)
            try:
                export_name, string_pos = read_net_string(data, payload_start + 10, end)
                file_name, _ = read_net_string(data, string_pos, end)
            except ValueError:
                file_name = ""
            if file_name.casefold() == TARGET.casefold():
                struct.pack_into("<HH", out, payload_start + 6, TARGET_WIDTH, TARGET_HEIGHT)
                patched.append((image_id, id_type, bitmap_format, width, height, export_name))
        pos = end
        if code == 0:
            break

    if len(patched) != 1:
        raise SystemExit(
            f"Loading screens: expected exactly one {TARGET} DefineExternalImage2, found {len(patched)}")

    # Strong invariant: same file length and only the four dimension bytes may differ.
    changed = [i for i, (a, b) in enumerate(zip(data, out)) if a != b]
    allowed = set()
    # Locate the target again to compute its exact four-byte dimension field.
    pos = header_end(data)
    while pos < len(data):
        code, payload_start, end = parse_tag(data, pos)
        if code == 1009 and end - payload_start >= 10:
            try:
                _, string_pos = read_net_string(data, payload_start + 10, end)
                file_name, _ = read_net_string(data, string_pos, end)
            except ValueError:
                file_name = ""
            if file_name.casefold() == TARGET.casefold():
                allowed.update(range(payload_start + 6, payload_start + 10))
                break
        pos = end
        if code == 0:
            break
    if any(i not in allowed for i in changed) or len(out) != len(data):
        raise SystemExit("Loading screens: aspect patch changed bytes outside the external-image dimensions")

    image_id, id_type, bitmap_format, old_w, old_h, _ = patched[0]
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_bytes(out)
    print(
        "Loading screens: classic GFX aspect patch "
        f"id={image_id} idType={id_type} format={bitmap_format} "
        f"{old_w}x{old_h} -> {TARGET_WIDTH}x{TARGET_HEIGHT}; "
        f"{len(changed)} byte(s) changed, structure/timeline unchanged"
    )


if __name__ == "__main__":
    main()
