"""Prepare the redistributable LGeneral runtime data using only Python's stdlib.

The original BMPs use several Windows headers, palettes and channel masks.  The
guest SDL subset deliberately accepts one canonical format: uncompressed BGR24
with a 40-octet DIB header.  Conversion preserves RGB values, including the font
metadata row and sprite colour-key pixels; alpha is not used by LGeneral.
"""

from __future__ import annotations

import hashlib
import json
import shutil
import struct
from pathlib import Path


RUNTIME_DIRS = (
    "campaigns", "gfx", "maps", "music", "nations", "scenarios", "sounds",
    "terrain", "themes", "units",
)
NAME_MAX = 30
PATH_MAX = 256
MAX_PIXELS = 16 * 1024 * 1024


def decode_bmp(data: bytes) -> tuple[int, int, bytes]:
    """Return width, height and top-to-bottom RGB octets for a Windows BMP."""
    if len(data) < 54 or data[:2] != b"BM":
        raise ValueError("not a Windows BMP")
    offset = struct.unpack_from("<I", data, 10)[0]
    header = struct.unpack_from("<I", data, 14)[0]
    if header not in (40, 52, 56, 108, 124) or len(data) < 14 + header:
        raise ValueError(f"unsupported or truncated BMP header: {header}")
    width, signed_height, planes, bits, compression = struct.unpack_from(
        "<iiHHI", data, 18
    )
    height = abs(signed_height)
    if width <= 0 or height == 0 or width * height > MAX_PIXELS or planes != 1:
        raise ValueError("invalid BMP dimensions or planes")
    if bits not in (1, 4, 8, 16, 24, 32) or compression not in (0, 3):
        raise ValueError(f"unsupported BMP format: {bits} bits, compression {compression}")
    if compression == 3 and bits not in (16, 24, 32):
        raise ValueError("BMP bitfields require true-colour pixels")
    stride = ((width * bits + 31) // 32) * 4
    if offset < 14 + header or offset + stride * height > len(data):
        raise ValueError("truncated BMP pixels")

    palette: list[tuple[int, int, int]] = []
    masks = (0xFF0000, 0x00FF00, 0x0000FF)
    if bits <= 8:
        used = struct.unpack_from("<I", data, 46)[0] or (1 << bits)
        if used > (1 << bits) or 14 + header + used * 4 > offset:
            raise ValueError("truncated or invalid BMP palette")
        for index in range(used):
            blue, green, red, _ = struct.unpack_from("<BBBB", data, 14 + header + index * 4)
            palette.append((red, green, blue))
    elif compression == 3:
        mask_offset = 14 + 40
        if mask_offset + 12 > offset:
            raise ValueError("truncated BMP channel masks")
        masks = struct.unpack_from("<III", data, mask_offset)
        if any(mask == 0 or mask >= (1 << bits) for mask in masks):
            raise ValueError("invalid BMP channel masks")
        if masks[0] & masks[1] or masks[0] & masks[2] or masks[1] & masks[2]:
            raise ValueError("overlapping BMP channel masks")
    elif bits == 16:
        masks = (0x7C00, 0x03E0, 0x001F)

    channels: list[tuple[int, int]] = []
    if bits >= 16:
        for mask in masks:
            shift = (mask & -mask).bit_length() - 1
            maximum = mask >> shift
            if maximum & (maximum + 1):
                raise ValueError("non-contiguous BMP channel mask")
            channels.append((shift, maximum))

    pixels = bytearray(width * height * 3)
    for y in range(height):
        source_y = y if signed_height < 0 else height - 1 - y
        source = offset + source_y * stride
        target = y * width * 3
        for x in range(width):
            if bits <= 8:
                octet = data[source + x * bits // 8]
                shift = 8 - bits - ((x * bits) % 8)
                index = (octet >> shift) & ((1 << bits) - 1)
                if index >= len(palette):
                    raise ValueError("BMP pixel exceeds palette")
                red, green, blue = palette[index]
            else:
                start = source + x * (bits // 8)
                value = int.from_bytes(data[start:start + bits // 8], "little")
                rgb = [(((value >> shift) & maximum) * 255 + maximum // 2) // maximum
                       for shift, maximum in channels]
                red, green, blue = rgb
            pixels[target:target + 3] = bytes((red, green, blue))
            target += 3
    return width, height, bytes(pixels)


def encode_bmp(width: int, height: int, rgb: bytes) -> bytes:
    """Encode canonical bottom-up BGR24, without lossy quantization."""
    if width <= 0 or height <= 0 or width * height > MAX_PIXELS or len(rgb) != width * height * 3:
        raise ValueError("invalid RGB dimensions")
    stride = (width * 3 + 3) & ~3
    size = stride * height
    output = bytearray(54 + size)
    struct.pack_into("<2sIHHI", output, 0, b"BM", len(output), 0, 0, 54)
    struct.pack_into("<IiiHHIIiiII", output, 14, 40, width, height, 1, 24, 0, size, 0, 0, 0, 0)
    for y in range(height):
        target = 54 + (height - 1 - y) * stride
        source = y * width * 3
        for x in range(width):
            red, green, blue = rgb[source:source + 3]
            output[target:target + 3] = bytes((blue, green, red))
            source += 3
            target += 3
    return bytes(output)


def normalize_bmp(data: bytes) -> bytes:
    return encode_bmp(*decode_bmp(data))


def audit_paths(root: Path, guest_root: str = "/usr/share/lgeneral") -> dict[str, int]:
    """Fail before image creation if a runtime pathname exceeds guest limits."""
    longest_name = 0
    longest_path = 0
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        for part in relative.parts:
            length = len(part.encode("utf-8"))
            longest_name = max(longest_name, length)
            if length > NAME_MAX:
                raise ValueError(f"guest filename exceeds {NAME_MAX} octets: {relative}")
        length = len((guest_root.rstrip("/") + "/" + relative.as_posix()).encode("utf-8"))
        longest_path = max(longest_path, length)
        if length >= PATH_MAX:
            raise ValueError(f"guest pathname exceeds {PATH_MAX - 1} octets: {relative}")
    return {"longest_name_octets": longest_name, "longest_path_octets": longest_path}


def prepare_assets(source_root: Path, data_root: Path, output: Path) -> dict:
    """Merge pristine engine assets and free K.u.k. data into a clean directory.

    Engine's PG campaign stub is omitted because it refers to commercial data.
    The content package also has unused editor maps in maps_without_scenerio;
    these have no scenario references and include names longer than NAME_MAX.
    """
    source_root, data_root, output = map(Path, (source_root, data_root, output))
    if not (source_root / "src/gfx/title.bmp").is_file() or not (data_root / "LICENSE").is_file():
        raise ValueError("expected pristine LGeneral engine and lgeneral-data roots")
    if any(root.resolve().is_relative_to(output.resolve())
           or output.resolve().is_relative_to(root.resolve())
           for root in (source_root, data_root)):
        raise ValueError("asset output must be separate from source roots")
    if output.exists():
        shutil.rmtree(output)
    output.mkdir(parents=True)
    for directory in RUNTIME_DIRS:
        (output / directory).mkdir()

    files: dict[str, dict] = {}
    omitted: list[str] = []

    def copy_asset(path: Path, relative: Path, origin: str) -> None:
        if path.is_symlink():
            raise ValueError(f"unexpected symlink in upstream assets: {path}")
        data = path.read_bytes()
        original_hash = hashlib.sha256(data).hexdigest()
        dimensions = None
        if path.suffix.lower() == ".bmp":
            width, height, rgb = decode_bmp(data)
            data = encode_bmp(width, height, rgb)
            dimensions = [width, height]
        target = output / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(data)
        target.chmod(0o644)
        files[relative.as_posix()] = {
            "origin": origin, "source_sha256": original_hash,
            "sha256": hashlib.sha256(data).hexdigest(), "octets": len(data),
            **({"dimensions": dimensions} if dimensions else {}),
        }

    for directory in ("gfx", "themes"):
        for path in sorted((source_root / "src" / directory).rglob("*")):
            if path.is_file() and path.suffix.lower() in (".bmp", ".wav"):
                copy_asset(path, path.relative_to(source_root / "src"), "lgeneral-1.4.4")
    for directory in RUNTIME_DIRS:
        if not (data_root / directory).exists():
            continue
        for path in sorted((data_root / directory).rglob("*")):
            if not path.is_file():
                continue
            relative = path.relative_to(data_root)
            if "maps_without_scenerio" in relative.parts:
                omitted.append(relative.as_posix())
                continue
            copy_asset(path, relative, "lgeneral-data-1.1.1")

    notices = (
        (source_root / "COPYING", "engine-COPYING"),
        (source_root / "AUTHORS", "engine-AUTHORS"),
        (source_root / "src/themes/default/COPYING", "theme-COPYING"),
        (data_root / "COPYING", "kukgen-COPYING"),
        (data_root / "LICENSE", "kukgen-LICENSE"),
        (data_root / "THANKS", "kukgen-THANKS"),
        (data_root / "README", "kukgen-README"),
    )
    for path, name in notices:
        copy_asset(path, Path("licenses") / name, "upstream notice")
    report = {
        "format": 1, "bmp_format": "Windows V3 BGR24, uncompressed",
        "files": dict(sorted(files.items())), "omitted_unused_maps": omitted,
        "file_count": len(files), "bmp_count": sum("dimensions" in record for record in files.values()),
        "octets": sum(record["octets"] for record in files.values()),
        "path_limits": audit_paths(output),
    }
    manifest = output / "assets-manifest.json"
    manifest.write_text(json.dumps(report, indent=2, sort_keys=True) + "\n")
    manifest.chmod(0o644)
    audit_paths(output)
    return report
