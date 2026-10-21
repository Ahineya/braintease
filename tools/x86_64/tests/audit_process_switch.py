#!/usr/bin/env python3
"""Read-only source/layout audit; this does not time or switch an x64 process.

Extract the production runtime's actual type declarations and ask Clang for
their x86_64 freestanding layouts. Keep synthetic copy-work examples separate
from those compiler results. Emit a small JSON report to stdout; do not build a
kernel, execute guest code, modify page tables, or write files.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


ROOT = Path(__file__).resolve().parents[3]
PLATFORM = ROOT / "tools/x86_64"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="clang", help="Clang executable")
    args = parser.parse_args()
    runtime = (PLATFORM / "boot/runtime.c").read_text()
    begin = runtime.index("#define BANK ")
    end = runtime.index("static Context context;")
    declarations = runtime[begin:end]
    fields = {
        "bank_cells": "BANK",
        "total_cells": "CELLS",
        "private_cells": "KERNEL_START",
        "legacy_page_cells": "PAGE_CELLS",
        "legacy_private_pages": "PRIVATE_PAGES",
        "process_slots": "PROCESSES",
        "continuation_capacity": "FRAMES",
        "frame_bytes": "sizeof(Frame)",
        "heap_bytes": "sizeof(Heap)",
        "ram_page_bytes": "sizeof(RamPage)",
        "ram_page_cells_offset": "offsetof(RamPage, cells)",
        "process_bytes": "sizeof(Process)",
        "context_bytes": "sizeof(Context)",
        "context_alignment": "_Alignof(Context)",
        "model_metadata_bytes": "sizeof(X64PmSpace)",
        "model_page_bytes": "X64_PM_PAGE_BYTES",
        "model_page_cells": "X64_PM_PAGE_CELLS",
        "model_private_pages": "X64_PM_PRIVATE_PAGES",
        "model_total_pages": "X64_PM_TOTAL_PAGES",
    }
    pm_header = PLATFORM / "process-memory/process_memory.h"
    probe = "#include <stddef.h>\n#include <stdint.h>\n"
    probe += "#include " + json.dumps(str(pm_header)) + "\n" + declarations
    probe += "\n".join(
        f"const uint64_t audit_{name} = {expression};"
        for name, expression in fields.items()
    )
    command = [
        args.cc, "--target=x86_64-unknown-none-elf", "-std=c11",
        "-ffreestanding", "-fsyntax-only", "-x", "c", "-",
    ]
    # First diagnose malformed declarations separately from LLVM text parsing.
    subprocess.run(command, input=probe, text=True, check=True, capture_output=True)
    command[command.index("-fsyntax-only"):command.index("-fsyntax-only") + 1] = [
        "-S", "-emit-llvm", "-o", "-",
    ]
    output = subprocess.run(
        command, input=probe, text=True, check=True, capture_output=True,
    ).stdout
    values = {
        name: int(value)
        for name, value in re.findall(
            r"^@audit_(\w+)\s*=.*?\bconstant i64 (\d+)\b", output, re.MULTILINE,
        )
    }
    if set(values) != set(fields):
        raise RuntimeError("Clang audit constants missing; no layout was inferred")

    process_source = (PLATFORM / "boot/process.inc").read_text()
    for required in (
        "i < PRIVATE_PAGES; ++i) if (c->dirty[i])",
        "previous->pages[i] == next->pages[i]",
        "c->depth * sizeof(Frame)", "sizeof(Heap)",
    ):
        if required not in process_source:
            raise RuntimeError("Snapshot implementation changed; review work formulas")

    v = values
    private_bytes = v["private_cells"] * 2
    old_payload = v["legacy_page_cells"] * 2
    old_frames = v["legacy_private_pages"]
    new_frames = v["model_private_pages"]

    def work_example(name: str, dirty_pages: int, nonzero_pages: int,
                     differing_pages: int, old_depth: int, new_depth: int) -> dict:
        return {
            "name": name,
            "fixture_inputs": {
                "dirty_pages": dirty_pages, "dirty_nonzero_pages": nonzero_pages,
                "different_snapshot_identities": differing_pages,
                "old_depth": old_depth, "new_depth": new_depth,
            },
            "save_dirty_flags_examined": old_frames,
            "save_cell_values_examined": dirty_pages * v["legacy_page_cells"],
            "save_payload_bytes_copied": nonzero_pages * old_payload,
            "restore_pointer_pairs_examined": old_frames,
            "restore_payload_bytes_copied_or_zeroed": differing_pages * old_payload,
            "continuation_and_heap_bytes_copied": (
                (old_depth + new_depth) * v["frame_bytes"] + 2 * v["heap_bytes"]
            ),
        }

    sources = (
        "boot/runtime.c", "boot/process.inc", "boot/kernel.c", "boot/kernel.h",
        "boot/arch.S", "boot/timer.c", "process-memory/process_memory.h",
        "process-memory/process_memory.c",
    )
    report = {
        "kind": "source_layout_and_synthetic_work_audit",
        "target": "x86_64-unknown-none-elf",
        "compiler": subprocess.run(
            [args.cc, "--version"], text=True, capture_output=True, check=True,
        ).stdout.splitlines()[0],
        "production_or_guest_executed": False,
        "live_mappings_changed": False,
        "timing_measured": False,
        "compiler_measured_layout": values,
        "source_derived_capacity": {
            "active_cell_array_bytes": v["total_cells"] * 2,
            "private_payload_bytes": private_bytes,
            "legacy_full_backing_allocation_bytes": old_frames * 4096,
            "legacy_full_backing_refcount_bytes": (
                old_frames * v["ram_page_cells_offset"]
            ),
            "legacy_full_backing_unused_allocation_bytes": (
                old_frames * (4096 - v["ram_page_bytes"])
            ),
            "legacy_process_allocation_bytes": (
                (v["process_bytes"] + 4095) // 4096 * 4096
            ),
            "model_full_backing_allocation_bytes": new_frames * v["model_page_bytes"],
            "model_plan_descriptors": v["model_total_pages"],
            "model_plan_payload_bytes_copied": 0,
        },
        "synthetic_examples_not_runtime_observations": [
            work_example("clean_same_backing_depth_1", 0, 0, 0, 1, 1),
            work_example("one_dirty_nonzero_different_page_depth_1", 1, 1, 1, 1, 1),
            work_example(
                "full_dirty_nonzero_different_arena_max_depth", old_frames,
                old_frames, old_frames, v["continuation_capacity"],
                v["continuation_capacity"],
            ),
        ],
        "limitations": [
            "Source-derived loop work is not elapsed time, cache traffic or a speedup.",
            "Examples assume a successful save and the stated fixture inputs.",
            "Actual runtime dirty-page counts and continuation depths were not sampled.",
            "The backing prototype remains isolated; plan descriptors are not PTEs.",
        ],
        "source_sha256": {
            str(Path("tools/x86_64") / relative): hashlib.sha256(
                (PLATFORM / relative).read_bytes()
            ).hexdigest()
            for relative in sources
        },
    }
    print(json.dumps(report, indent=2, sort_keys=True))


if __name__ == "__main__":
    main()
