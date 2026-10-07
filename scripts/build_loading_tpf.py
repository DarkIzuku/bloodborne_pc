#!/usr/bin/env python3
"""Build a validated Bloodborne classic-loading TPF with six packaged custom textures.

This deliberately delegates PS4 TPF/DDS repacking to WitchyBND/SoulsFormatsNEXT instead of
reimplementing FromSoftware's PS4 swizzle and padding rules. The user's dump is never modified:
we unpack a temporary copy of dvdroot_ps4/menu/nowloading.tpf.dcx, append six textures cloned from
MENU_NowLoading_00001's metadata, repack it, validate the result by unpacking it again, and write
only the port-owned output under out/.
"""
import argparse
import copy
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
    28: "R8G8B8A8_UNORM",
    29: "R8G8B8A8_UNORM_SRGB",
    65: "A8_UNORM",
    71: "BC1_UNORM",
    72: "BC1_UNORM_SRGB",
    74: "BC2_UNORM",
    75: "BC2_UNORM_SRGB",
    77: "BC3_UNORM",
    78: "BC3_UNORM_SRGB",
    80: "BC4_UNORM",
    81: "BC4_SNORM",
    83: "BC5_UNORM",
    84: "BC5_SNORM",
    87: "B8G8R8A8_UNORM",
    91: "B8G8R8A8_UNORM_SRGB",
    95: "BC6H_UF16",
    96: "BC6H_SF16",
    98: "BC7_UNORM",
    99: "BC7_UNORM_SRGB",
}


def run_tool(command, label):
    result = subprocess.run(
        [str(x) for x in command],
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    if result.returncode:
        output = result.stdout.strip()
        raise ValueError(f"{label} failed with exit code {result.returncode}: {output}")
    return result.stdout


def unpack_dir_for(path):
    name = path.name.replace(".", "-")
    if name == path.name:
        name += "-unpacked"
    return path.parent / name


def parse_target_metadata(source):
    """Read only stable PS4 TPF metadata; WitchyBND performs all actual texture packing."""
    data = source.read_bytes()
    if len(data) < 76 or data[:4] != b"DCX\0" or data[40:44] != b"DFLT":
        raise ValueError("classic loading texture package is not the expected DCX_DFLT container")
    import zlib
    compressed_size = struct.unpack_from(">I", data, 32)[0]
    tpf = zlib.decompress(data[76:76 + compressed_size])
    if len(tpf) < 16 or tpf[:4] != b"TPF\0":
        raise ValueError("decompressed classic loading texture package is not a TPF")
    count = struct.unpack_from("<I", tpf, 8)[0]
    platform, encoding = tpf[12], tpf[14]
    if platform != 4:
        raise ValueError(f"expected PS4 TPF platform 4, found {platform}")
    pos = 16
    records = []
    for index in range(count):
        if pos + 36 > len(tpf):
            raise ValueError(f"truncated PS4 TPF record {index}")
        start = pos
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
                raise ValueError(f"unterminated TPF texture name in record {index}")
            name = tpf[name_offset:end].decode("shift_jis", errors="replace")
        records.append({
            "name": name,
            "format": fmt,
            "type": texture_type,
            "mips": mips,
            "flags1": flags1,
            "width": width,
            "height": height,
            "texture_count": texture_count,
            "unk2": unk2,
            "dxgi": dxgi,
            "file_size": file_size,
            "record_size": pos - start,
            "file_offset": file_offset,
        })
    target = next((r for r in records if r["name"].casefold() == TARGET_STEM.casefold()), None)
    if target is None:
        raise ValueError(
            f"{TARGET_STEM} not found in loading TPF; textures: " +
            ", ".join(r["name"] for r in records)
        )
    return target, len(records)


def texture_name(node):
    value = node.findtext("name")
    return Path(value).stem if value else ""


def find_manifest_target(root):
    textures = root.find("textures")
    if textures is None:
        raise ValueError("WitchyBND TPF manifest has no <textures> section")
    for node in textures.findall("texture"):
        if texture_name(node).casefold() == TARGET_STEM.casefold():
            return textures, node
    names = [texture_name(node) for node in textures.findall("texture")]
    raise ValueError(f"{TARGET_STEM} missing from WitchyBND manifest; textures: {', '.join(names)}")


def make_dds(image, output, texconv, target):
    try:
        fmt = DXGI_TO_TEXCONV[target["dxgi"]]
    except KeyError:
        raise ValueError(f"unsupported loading texture DXGI {target['dxgi']}")
    with tempfile.TemporaryDirectory(prefix="bb-loading-dds-") as temp:
        temp = Path(temp)
        command = [
            texconv, "-y", "-nologo",
            "-f", fmt,
            "-w", str(target["width"]),
            "-h", str(target["height"]),
            "-m", str(target["mips"] if target["mips"] else 1),
            "-o", temp,
            image,
        ]
        run_tool(command, f"texconv {image.name}")
        candidates = list(temp.glob(image.stem + ".DDS")) + list(temp.glob(image.stem + ".dds"))
        if not candidates:
            candidates = list(temp.glob("*.DDS")) + list(temp.glob("*.dds"))
        if not candidates:
            raise ValueError(f"texconv produced no DDS for {image.name}")
        shutil.copy2(candidates[0], output)


def witchy(witchy_exe, mode, path):
    flag = "--unpack" if mode == "unpack" else "--repack"
    # --silent is also passive, so WitchyBND never prompts, pauses or performs update checks.
    run_tool([witchy_exe, "--silent", "--singlethread", flag, path],
             f"WitchyBND {mode} {Path(path).name}")


def validate_repacked(witchy_exe, packed, expected_names):
    with tempfile.TemporaryDirectory(prefix="bb-loading-validate-") as temp:
        temp = Path(temp)
        probe = temp / "verify.tpf.dcx"
        shutil.copy2(packed, probe)
        witchy(witchy_exe, "unpack", probe)
        unpacked = unpack_dir_for(probe)
        manifest = unpacked / "_witchy-tpf.xml"
        if not manifest.is_file():
            raise ValueError("WitchyBND validation did not produce _witchy-tpf.xml")
        root = ET.parse(manifest).getroot()
        names = {
            texture_name(node).casefold()
            for node in root.findall("./textures/texture")
        }
        missing = [name for name in expected_names if name.casefold() not in names]
        if missing:
            raise ValueError("repacked TPF is missing custom textures: " + ", ".join(missing))


def fingerprint(paths, tools):
    digest = hashlib.sha256()
    for path in paths:
        digest.update(path.name.encode("utf-8"))
        digest.update(path.read_bytes())
    for tool in tools:
        stat = tool.stat()
        digest.update(tool.name.encode("utf-8"))
        digest.update(str(stat.st_size).encode("ascii"))
    return digest.hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("--images-dir", required=True, type=Path)
    parser.add_argument("--texconv", required=True, type=Path)
    parser.add_argument("--witchy", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    images = sorted([*args.images_dir.glob("loading_*.jpg"),
                     *args.images_dir.glob("loading_*.jpeg")])
    if len(images) != EXPECTED_COUNT:
        raise SystemExit(f"Expected {EXPECTED_COUNT} loading backgrounds, found {len(images)}")
    for path in [args.source, args.texconv, args.witchy, *images]:
        if not path.is_file():
            raise SystemExit(f"Missing loading-screen input/tool: {path}")

    key = fingerprint([Path(__file__), args.source, *images], [args.texconv, args.witchy])
    stamp = args.output.with_suffix(args.output.suffix + ".sha256")
    if args.output.is_file() and stamp.is_file() and stamp.read_text().strip() == key:
        print("Loading screens: cached WitchyBND-validated classic TPF with 6 custom textures")
        return

    try:
        target, original_count = parse_target_metadata(args.source)
        if target["type"] != 0:
            raise ValueError(f"classic loading texture is not 2D (type={target['type']})")
        if target["dxgi"] not in DXGI_TO_TEXCONV:
            raise ValueError(f"unsupported classic loading texture DXGI {target['dxgi']}")
        print(
            "Loading screens: authoritative PS4 TPF path via WitchyBND/SoulsFormatsNEXT; "
            f"target={TARGET_STEM} {target['width']}x{target['height']} "
            f"dxgi={target['dxgi']} mips={target['mips']} flags={target['flags1']} "
            f"unk2=0x{target['unk2']:X}; original records={original_count}"
        )

        with tempfile.TemporaryDirectory(prefix="bb-loading-tpf-") as temp:
            temp = Path(temp)
            working = temp / "nowloading.tpf.dcx"
            shutil.copy2(args.source, working)
            witchy(args.witchy, "unpack", working)
            unpacked = unpack_dir_for(working)
            manifest = unpacked / "_witchy-tpf.xml"
            if not manifest.is_file():
                raise ValueError("WitchyBND did not produce _witchy-tpf.xml")

            tree = ET.parse(manifest)
            root = tree.getroot()
            textures, target_node = find_manifest_target(root)
            expected = []
            for index, image in enumerate(images, start=1):
                stem = f"BB_Loading_{index:02d}"
                expected.append(stem)
                new_node = copy.deepcopy(target_node)
                name = new_node.find("name")
                if name is None:
                    raise ValueError("target texture manifest node has no <name>")
                name.text = stem + ".dds"
                textures.append(new_node)
                make_dds(image, unpacked / (stem + ".dds"),
                         args.texconv, target)

            tree.write(manifest, encoding="utf-8", xml_declaration=True)
            # Avoid WitchyBND's backup path and ensure the repack result is newly created.
            working.unlink()
            witchy(args.witchy, "repack", unpacked)
            if not working.is_file():
                raise ValueError("WitchyBND repack did not recreate nowloading.tpf.dcx")

            validate_repacked(args.witchy, working, expected)
            args.output.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(working, args.output)

    except (OSError, ValueError, ET.ParseError, struct.error) as error:
        raise SystemExit(f"Loading screens: TPF generation failed: {error}")

    stamp.write_text(key + "\n")
    print("Loading screens: WitchyBND validated original resources + BB_Loading_01..06; "
          "original game files unchanged")


if __name__ == "__main__":
    main()
