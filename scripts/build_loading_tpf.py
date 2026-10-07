#!/usr/bin/env python3
"""Build six validated Bloodborne classic-loading TPF variants.

Each variant keeps the original TPF record names and metadata intact and replaces only the DDS
payload of MENU_NowLoading_00001. WitchyBND/SoulsFormatsNEXT performs the PS4 texture swizzle and
TPF/DCX repack. The user's dump is read-only; outputs live under out/ui/loading_screens.
"""
import argparse
import hashlib
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import xml.etree.ElementTree as ET

EXPECTED_COUNT = 6
TARGET_STEM = "MENU_NowLoading_00001"

DXGI_TO_TEXCONV = {
    28: "R8G8B8A8_UNORM", 29: "R8G8B8A8_UNORM_SRGB", 65: "A8_UNORM",
    71: "BC1_UNORM", 72: "BC1_UNORM_SRGB", 74: "BC2_UNORM", 75: "BC2_UNORM_SRGB",
    77: "BC3_UNORM", 78: "BC3_UNORM_SRGB", 80: "BC4_UNORM", 81: "BC4_SNORM",
    83: "BC5_UNORM", 84: "BC5_SNORM", 87: "B8G8R8A8_UNORM", 91: "B8G8R8A8_UNORM_SRGB",
    95: "BC6H_UF16", 96: "BC6H_SF16", 98: "BC7_UNORM", 99: "BC7_UNORM_SRGB",
}


def run_tool(command, label):
    result = subprocess.run(
        [str(x) for x in command], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
        text=True, encoding="utf-8", errors="replace")
    if result.returncode:
        raise ValueError(f"{label} failed with exit code {result.returncode}: {result.stdout.strip()}")
    return result.stdout


def unpack_dir_for(path):
    name = path.name.replace(".", "-")
    if name == path.name:
        name += "-unpacked"
    return path.parent / name


def witchy(exe, mode, path):
    flag = "--unpack" if mode == "unpack" else "--repack"
    run_tool([exe, "--silent", "--singlethread", flag, path],
             f"WitchyBND {mode} {Path(path).name}")


def parse_target_metadata(source):
    data = source.read_bytes()
    if len(data) < 76 or data[:4] != b"DCX\0" or data[40:44] != b"DFLT":
        raise ValueError("classic loading package is not the expected DCX_DFLT container")
    import zlib
    compressed_size = struct.unpack_from(">I", data, 32)[0]
    tpf = zlib.decompress(data[76:76 + compressed_size])
    if len(tpf) < 16 or tpf[:4] != b"TPF\0":
        raise ValueError("decompressed loading package is not a TPF")
    count = struct.unpack_from("<I", tpf, 8)[0]
    platform, encoding = tpf[12], tpf[14]
    if platform != 4:
        raise ValueError(f"expected PS4 TPF platform 4, found {platform}")
    pos = 16
    records = []
    for index in range(count):
        if pos + 36 > len(tpf):
            raise ValueError(f"truncated PS4 TPF record {index}")
        file_offset, file_size = struct.unpack_from("<Ii", tpf, pos)
        fmt, texture_type, mips, flags1 = tpf[pos + 8:pos + 12]
        width, height = struct.unpack_from("<hh", tpf, pos + 12)
        texture_count, unk2, name_offset, has_float, dxgi = struct.unpack_from("<iiIii", tpf, pos + 16)
        pos += 36
        if has_float:
            if pos + 8 > len(tpf):
                raise ValueError(f"truncated float metadata in record {index}")
            _, float_size = struct.unpack_from("<ii", tpf, pos)
            if float_size < 0 or pos + 8 + float_size > len(tpf):
                raise ValueError(f"invalid float metadata in record {index}")
            pos += 8 + float_size
        if encoding == 1:
            end = name_offset
            while end + 1 < len(tpf) and tpf[end:end + 2] != b"\0\0":
                end += 2
            name = tpf[name_offset:end].decode("utf-16-le", errors="replace")
        else:
            end = tpf.find(b"\0", name_offset)
            if end < 0:
                raise ValueError(f"unterminated TPF name in record {index}")
            name = tpf[name_offset:end].decode("shift_jis", errors="replace")
        records.append({
            "name": name, "format": fmt, "type": texture_type, "mips": mips, "flags1": flags1,
            "width": width, "height": height, "texture_count": texture_count, "unk2": unk2,
            "dxgi": dxgi, "file_size": file_size, "file_offset": file_offset,
        })
    target = next((r for r in records if r["name"].casefold() == TARGET_STEM.casefold()), None)
    if target is None:
        raise ValueError(f"{TARGET_STEM} not found; textures: " + ", ".join(r["name"] for r in records))
    return target, len(records)


def texture_name(node):
    value = node.findtext("name")
    return Path(value).stem if value else ""


def find_target_node(root):
    textures = root.find("textures")
    if textures is None:
        raise ValueError("WitchyBND manifest has no <textures>")
    for node in textures.findall("texture"):
        if texture_name(node).casefold() == TARGET_STEM.casefold():
            return node
    raise ValueError(f"{TARGET_STEM} missing from WitchyBND manifest")


def make_dds(image, output, texconv, target):
    try:
        fmt = DXGI_TO_TEXCONV[target["dxgi"]]
    except KeyError:
        raise ValueError(f"unsupported loading texture DXGI {target['dxgi']}")
    with tempfile.TemporaryDirectory(prefix="bb-loading-dds-") as td:
        td = Path(td)
        run_tool([
            texconv, "-y", "-nologo", "-f", fmt,
            "-w", str(target["width"]), "-h", str(target["height"]),
            "-m", str(target["mips"] if target["mips"] else 1),
            "-o", td, image,
        ], f"texconv {image.name}")
        candidates = list(td.glob(image.stem + ".DDS")) + list(td.glob(image.stem + ".dds"))
        if not candidates:
            candidates = list(td.glob("*.DDS")) + list(td.glob("*.dds"))
        if not candidates:
            raise ValueError(f"texconv produced no DDS for {image.name}")
        shutil.copy2(candidates[0], output)


def validate_variant(witchy_exe, packed, expected_target, original_count):
    with tempfile.TemporaryDirectory(prefix="bb-loading-verify-") as td:
        td = Path(td)
        probe = td / "verify.tpf.dcx"
        shutil.copy2(packed, probe)
        witchy(witchy_exe, "unpack", probe)
        unpacked = unpack_dir_for(probe)
        manifest = unpacked / "_witchy-tpf.xml"
        if not manifest.is_file():
            raise ValueError("validation unpack produced no _witchy-tpf.xml")
        root = ET.parse(manifest).getroot()
        textures = root.findall("./textures/texture")
        if len(textures) != original_count:
            raise ValueError(f"TPF record count changed ({len(textures)} != {original_count})")
        node = find_target_node(root)
        name = node.findtext("name")
        target_dds = unpacked / Path(name).name
        if not target_dds.is_file():
            raise ValueError("validation target DDS is missing")
        if target_dds.stat().st_size <= 128:
            raise ValueError("validation target DDS is empty")
        if texture_name(node).casefold() != expected_target.casefold():
            raise ValueError("validation target name changed")


def fingerprint(paths, tools):
    h = hashlib.sha256()
    for path in paths:
        h.update(path.name.encode("utf-8"))
        h.update(path.read_bytes())
    for tool in tools:
        stat = tool.stat()
        h.update(tool.name.encode("utf-8"))
        h.update(str(stat.st_size).encode("ascii"))
    return h.hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("source", type=Path)
    p.add_argument("--images-dir", required=True, type=Path)
    p.add_argument("--texconv", required=True, type=Path)
    p.add_argument("--witchy", required=True, type=Path)
    p.add_argument("--out-dir", required=True, type=Path)
    args = p.parse_args()

    images = sorted([*args.images_dir.glob("loading_*.jpg"), *args.images_dir.glob("loading_*.jpeg")])
    if len(images) != EXPECTED_COUNT:
        raise SystemExit(f"Expected {EXPECTED_COUNT} loading backgrounds, found {len(images)}")
    for path in [args.source, args.texconv, args.witchy, *images]:
        if not path.is_file():
            raise SystemExit(f"Missing loading-screen input/tool: {path}")

    outputs = [args.out_dir / f"nowloading-custom-{i:02d}.tpf.dcx" for i in range(1, 7)]
    key = fingerprint([Path(__file__), args.source, *images], [args.texconv, args.witchy])
    stamp = args.out_dir / "loading-tpf.sha256"
    if all(x.is_file() for x in outputs) and stamp.is_file() and stamp.read_text().strip() == key:
        print("Loading screens: cached 6 validated original-name TPF variants")
        return

    try:
        target, original_count = parse_target_metadata(args.source)
        if target["type"] != 0:
            raise ValueError(f"classic loading texture is not 2D (type={target['type']})")
        print(
            "Loading screens: replacing original TPF texture in-place via WitchyBND/SoulsFormatsNEXT; "
            f"target={TARGET_STEM} {target['width']}x{target['height']} dxgi={target['dxgi']} "
            f"mips={target['mips']} flags={target['flags1']} unk2=0x{target['unk2']:X}; "
            f"records={original_count}"
        )
        args.out_dir.mkdir(parents=True, exist_ok=True)

        with tempfile.TemporaryDirectory(prefix="bb-loading-base-") as td:
            td = Path(td)
            base_file = td / "nowloading.tpf.dcx"
            shutil.copy2(args.source, base_file)
            witchy(args.witchy, "unpack", base_file)
            base_dir = unpack_dir_for(base_file)
            manifest = base_dir / "_witchy-tpf.xml"
            if not manifest.is_file():
                raise ValueError("WitchyBND did not produce _witchy-tpf.xml")
            root = ET.parse(manifest).getroot()
            target_node = find_target_node(root)
            target_name = target_node.findtext("name")
            if not target_name:
                raise ValueError("target texture has no manifest filename")
            target_filename = Path(target_name).name

            for index, (image, output) in enumerate(zip(images, outputs), start=1):
                work_parent = td / f"variant-{index:02d}"
                work_parent.mkdir()
                working = work_parent / "nowloading.tpf.dcx"
                unpacked = work_parent / unpack_dir_for(working).name
                shutil.copytree(base_dir, unpacked)
                make_dds(image, unpacked / target_filename, args.texconv, target)
                witchy(args.witchy, "repack", unpacked)
                if not working.is_file():
                    raise ValueError(f"variant {index}: WitchyBND did not create nowloading.tpf.dcx")
                validate_variant(args.witchy, working, TARGET_STEM, original_count)
                shutil.copy2(working, output)
                print(f"Loading screens: validated variant {index}/6 -> {output.name}")

    except (OSError, ValueError, ET.ParseError, struct.error) as error:
        raise SystemExit(f"Loading screens: TPF generation failed: {error}")

    stamp.write_text(key + "\n")
    print("Loading screens: generated 6 validated TPF variants using the original resource name; "
          "original game files unchanged")


if __name__ == "__main__":
    main()
