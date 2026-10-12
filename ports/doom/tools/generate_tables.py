#!/usr/bin/env python3
"""Generate bounded C initializers for Doom's canonical trigonometric tables.

Usage: generate_tables.py upstream/doomgeneric/tables.c build/generated
Compile the generated .c files and upstream tables.c with
DOOM_GENERATED_TABLES=1. Each helper initializes at most 64 elements. There are
no large static C arrays and no dynamically generated trigonometric values.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import struct


TABLES = (
    ("finetangent", "fixed_t", 4096),
    ("finesine", "fixed_t", 10240),
    ("tantoangle", "angle_t", 2049),
)
CHUNK_SIZE = 64
FUNCTIONS_PER_FILE = 8
BANNER = """/* Generated from Doom's canonical tables by generate_tables.py.
 * Original tables: id Software / Simon Howard, GPL-2.0-or-later.
 * See the upstream COPYING/LICENSE and tables.c copyright notice.
 * Do not edit; regenerate from the pinned upstream source. */
#include "tables.h"

"""


def read_values(source: str, name: str, count: int) -> list[int]:
    pattern = rf"const\s+\w+\s+{name}\s*\[\s*{count}\s*\]\s*=\s*\{{(.*?)\}}\s*;"
    match = re.search(pattern, source, flags=re.DOTALL)
    if not match:
        raise ValueError(f"cannot locate canonical {name}[{count}] initializer")
    body = re.sub(r"/\*.*?\*/|//[^\n]*", "", match.group(1), flags=re.DOTALL)
    values = [int(value.strip().rstrip("uUlL"), 0) for value in body.split(",") if value.strip()]
    if len(values) != count:
        raise ValueError(f"{name}: expected {count} values, found {len(values)}")
    lower, upper = (0, 0xFFFFFFFF) if name == "tantoangle" else (-0x80000000, 0x7FFFFFFF)
    if any(not lower <= value <= upper for value in values):
        raise ValueError(f"{name}: a value is outside its 32-bit type")
    return values


def literal(value: int, ctype: str) -> str:
    if ctype == "angle_t":
        return f"UINT32_C({value})"
    if value == -0x80000000:
        return "INT32_MIN"
    if value < 0:
        return f"(-INT32_C({-value}))"
    return f"INT32_C({value})"


def generate(source_path: Path, output: Path) -> dict:
    source_bytes = source_path.read_bytes()
    source = source_bytes.decode("utf-8")
    output.mkdir(parents=True, exist_ok=True)
    functions: list[str] = []
    prototypes: list[str] = []
    calls: dict[str, list[str]] = {}
    manifest = {
        "format_version": 1,
        "source_sha256": hashlib.sha256(source_bytes).hexdigest(),
        "chunk_elements": CHUNK_SIZE,
        "tables": {},
        "sources": [],
    }
    for name, ctype, count in TABLES:
        values = read_values(source, name, count)
        packed = b"".join(struct.pack("<I", value & 0xFFFFFFFF) for value in values)
        fnv = 2166136261
        for octet in packed:
            fnv = ((fnv ^ octet) * 16777619) & 0xFFFFFFFF
        manifest["tables"][name] = {
            "entries": count,
            "sha256_le32": hashlib.sha256(packed).hexdigest(),
            "fnv1a_le32": f"{fnv:08x}",
        }
        calls[name] = []
        for index in range(0, count, CHUNK_SIZE):
            symbol = f"Doom_Fill_{name}_{index // CHUNK_SIZE:03d}"
            prototypes.append(f"{ctype} *{symbol}({ctype} *out);")
            body = [f"{ctype} *{symbol}({ctype} *out)", "{"]
            for value in values[index:index + CHUNK_SIZE]:
                body.append(f"    *out++ = {literal(value, ctype)};")
            body.extend(["    return out;", "}", ""])
            functions.append("\n".join(body))
            calls[name].append(f"    cursor = {symbol}(cursor);")

    generated = {}
    for index in range(0, len(functions), FUNCTIONS_PER_FILE):
        filename = f"tables_fill_{index // FUNCTIONS_PER_FILE:03d}.c"
        generated[filename] = BANNER + "\n".join(functions[index:index + FUNCTIONS_PER_FILE])

    driver = [BANNER, "#include <stdlib.h>\n", *prototypes, "", "const fixed_t *finesine;",
              "const fixed_t *finetangent;", "const fixed_t *finecosine;",
              "const angle_t *tantoangle;", ""]
    for name, ctype, _count in TABLES:
        driver.extend([f"static void Init_{name}(void)", "{",
                       f"    {ctype} *cursor = ({ctype} *) {name};",
                       *calls[name], "    (void) cursor;", "}", ""])
    driver.extend([
        "void Doom_FreeTables(void)", "{",
        "    free((void *) finesine);", "    free((void *) finetangent);",
        "    free((void *) tantoangle);", "    finesine = NULL;", "    finetangent = NULL;",
        "    finecosine = NULL;", "    tantoangle = NULL;", "}", "",
        "int Doom_InitTables(void)", "{",
        "    if (finesine != NULL && finetangent != NULL && tantoangle != NULL)",
        "        return 1;",
        "    Doom_FreeTables();",
        "    finesine = (const fixed_t *) malloc((size_t) 10240 * sizeof(fixed_t));",
        "    finetangent = (const fixed_t *) malloc((size_t) 4096 * sizeof(fixed_t));",
        "    tantoangle = (const angle_t *) malloc((size_t) 2049 * sizeof(angle_t));",
        "    if (finesine == NULL || finetangent == NULL || tantoangle == NULL)", "    {",
        "        Doom_FreeTables();", "        return 0;", "    }",
        "    Init_finetangent();", "    Init_finesine();", "    Init_tantoangle();",
        "    finecosine = finesine + FINEANGLES / 4;", "    return 1;", "}", "",
    ])
    generated["tables_init.c"] = "\n".join(driver)
    manifest["sources"] = sorted(generated)
    # Only replace files owned by this generator; unrelated output is retained.
    for previous in output.glob("tables_fill_*.c"):
        if previous.name not in generated:
            previous.unlink()
    for filename, content in generated.items():
        (output / filename).write_text(content)
    (output / "tables_manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    return manifest


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    manifest = generate(args.source, args.output)
    entries = sum(table["entries"] for table in manifest["tables"].values())
    print(f"Generated {entries} table entries in {len(manifest['sources'])} C files")


if __name__ == "__main__":
    main()
