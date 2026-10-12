"""Stage a WAD for the VM's fixed-size storage blocks."""
import hashlib
from pathlib import Path

BLOCK_OCTETS = 65536


def prepare_disk(wad: Path, disk: Path):
    if wad.resolve() == disk.resolve():
        raise ValueError("the VM disk image must differ from the source WAD")
    source = wad.read_bytes()
    # The current storage reader expects complete blocks. A short final block
    # reads as zero, so retain every WAD octet and pad only beyond logical EOF.
    image = source + bytes((-len(source)) % BLOCK_OCTETS)
    disk.write_bytes(image)
    return {"path": str(disk), "octets": len(image),
            "padding_octets": len(image) - len(source),
            "sha256": hashlib.sha256(image).hexdigest()}
