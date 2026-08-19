# rOS (DOS-like Ripple OS)

Launch:

```bash
rvm bios.bin --disk disks/ros.img
```

`rvm` is CPU + MMIO only. It never contains BIOS, kernel, or FAT code. `bios.bin` is a normal RLINK image loaded into IMEM bank 0.

Source: [`c-test/examples/rOS/`](../../c-test/examples/rOS/). Spec vs code: this page wins for the boot contract; device registers stay in [`mmio.md`](mmio.md).

## Layers

| Piece | Where | Role |
|-------|--------|------|
| BIOS | host `bios.bin` | Mount FAT16, `STORC` `IO.SYS`, jump. May use MMIO. Does not contain the shell. |
| IO.SYS | disk root | TTY + disk. Vector table at the image start. Owns MMIO. |
| KERNEL.SYS | disk root | FS, memory, exec, INT21. Loads `COMMAND.COM`. Never merged with IO or the shell. |
| COMMAND.COM | disk root | Shell (DIR/CD/TYPE/…). KERNEL APIs, not MMIO. |
| `*.RXE` | any directory | Relocatable user programs. |

Each SYS/COM file is a separate image. A layer can be tested with a stub for the next one (BIOS + stub IO, IO + stub KERNEL, KERNEL + stub COMMAND).

## IMEM / DMEM after boot

| Region | Use |
|--------|-----|
| IMEM 0 | `bios.bin` (`load_binary`) |
| IMEM 1 | `IO.SYS` (BIOS `STORC`) |
| IMEM 2 | `KERNEL.SYS` (IO `STORC`) |
| IMEM 3+ | `COMMAND.COM`, then apps (high banks, e.g. 32+) |
| DMEM 0 | MMIO (unchanged) |
| DMEM 1 | BIOS globals (`GP=1`), then unused |
| DMEM 2 | Kernel-mode stack (`SB=2`), IO + KERNEL + INT21 |
| DMEM 3 | IO globals (`GP=3`) |
| DMEM 4 | KERNEL globals (`GP=4`) |
| DMEM 5+ | Kernel-mode heap |
| DMEM 6 | COMMAND globals (`GP=6`) |
| DMEM 7 | COMMAND stack (`SB=7`) |
| High DMEM | App data/stack at `exec` |

IO owns MMIO. KERNEL talks to IO through the IO vector table (or INT21). COMMAND.COM and apps do not include `mmio.h`.

## SYS1 on-disk image

`rlink -f sys --code-bank-base N --gp-bank G --sb-bank S`. Little-endian. No DEBUG section.

```
offset  size  field
0       4     magic "SYS1"
4       2     bank_size
6       2     code_bank     (IMEM PCB to load at)
8       2     gp_bank
10      2     sb_bank
12      4     entry         (instruction index within the image; PC = entry, PCB = code_bank)
16      4     insn_count
20      4     data_size     (bytes)
24      …     instructions  (insn_count × 8 bytes, same layout as RLINK)
24+8n   …     data          (copied to DMEM[gp_bank][0…])
```

`--code-bank-base N` adds `N` to every `JAL` word2 after linking so intra-image calls are valid once the blob lives at PCB `N`. Data addresses stay offsets within the GP bank; the loader sets `GP`/`SB`/`SP`/`FP` before jumping. Loaded SYS crt0 must not overwrite those (BIOS uses ordinary [`runtime/crt0.asm`](../../runtime/crt0.asm) because `rvm` does not set them).

`STORC` grows IMEM: writing past the current instruction vector extends it with `NOP` (0x42), so a slim BIOS can load IO into bank 1.

## Vector tables

First object of IO.SYS / KERNEL.SYS is a trampoline table. Slot `k` is instruction `k` of that image (`JAL R0, R0, real_fn`). Callers `JALR RA, pcb, slot`. `JAL R0` does not clobber `RA`, so the real function `RET`s to the caller.

IO (PCB 1):

| Slot | Function |
|------|----------|
| 0 | `putchar` |
| 1 | `getchar` |
| 2 | `disk_read8` |
| 3 | `disk_write8` |
| 4 | `disk_read16` |
| 5 | `disk_read32` |
| 6 | `disk_read` |

KERNEL (PCB 2):

| Slot | Function |
|------|----------|
| 0 | `k_mount` |
| 1 | `k_lookup` (path in cwd) |
| 2 | `k_read_at` |
| 3 | `k_exec` |
| 4 | `k_exit` |
| 5 | `k_dir_open` / walk helpers as needed by the shell |

## RXE1 (`rlink -f rxe`)

Relocatable app. Linked as if code bank 0 / data offset 0. Loader picks free IMEM/DMEM banks, applies relocs, `STORC`s code, copies data, sets `GP`/`SB`/`SP`/`FP`, `JAL`s entry.

```
offset  size  field
0       4     magic "RXE1"
4       2     bank_size
6       2     reserved (0)
8       4     entry (in-image instruction index)
12      4     insn_count
16      4     data_size
20      4     reloc_count
24      …     instructions
…       …     data
…       …     relocs (8 bytes each)
```

Reloc record:

| Offset | Size | Field |
|--------|------|--------|
| 0 | 2 | kind: 0 `JalAbs`, 1 `LiData`, 2 `DataWord` |
| 2 | 2 | reserved |
| 4 | 4 | instruction index (`JalAbs`/`LiData`) or data-byte offset (`DataWord`) |

- **JalAbs** — add load-time code bank to `JAL` word2. word3 stays the in-bank offset.
- **LiData** — add load-time data base (offset within the GP bank; usually 0) to `LI` word2.
- **DataWord** — add the same data base to a 16-bit word stored in the data section.

App crt0 (`runtime/ros/crt0_ros.asm`) must not `HALT`. `main` return is INT21 exit back to COMMAND.COM. Keep `-f binary` / `rct` unchanged.

## INT21 (software IRQ)

Hardware: `IRQ_SW` at header 21–26 ([mmio.md](mmio.md)).

1. KERNEL at boot: `IRQ_VECTOR_*` = INT21 handler, `IRQ_ENABLE = IRQ_SW`.
2. App (via `libros`): args in `A0`–`A3`, write `IRQ_SW` to `IRQ_STATUS`.
3. Next instruction is interrupted; RA/RAB saved; handler runs on the **user stack** (v1).
4. Result in `RV0`/`RV1`; ACK `IRQ_BUSY`; `RET`.

`AH` in `A0`:

| AH | Name | Args | Result |
|----|------|------|--------|
| 0x01 | getchar | | `RV0` char or 0xFFFF EOF |
| 0x02 | putchar | `A1` char | |
| 0x09 | puts | `A1` addr, `A2` bank | |
| 0x3D | open | path fat-ptr | handle or −1 |
| 0x3F | read | handle, buf, len | bytes |
| 0x40 | write | handle, buf, len | bytes |
| 0x3E | close | handle | |
| 0x4B | exec | path fat-ptr | exit status |
| 0x4C | exit | `A1` status | does not return |

Do not enable overlapping STATUS bits from apps except `IRQ_SW`. Kernel `putchar` still hits TTY MMIO via IO.

## Runtime split

```
runtime/
  include/          shared headers
  common/           string, ctype, memcpy, memset, printf guts, softfloat, qfixed
  freestanding/     crt0.asm (HALT), mmio.c, MMIO putchar/getchar, bump malloc, graphics
  ros/              crt0_sys.asm (GP/SB already set), crt0_ros.asm (INT21 exit), INT21 wrappers
```

`libruntime.par` is the rct/freestanding archive. `libros.par` is apps + COMMAND. Include path stays `-I runtime/include`. `mmio.h` is BIOS/IO/KERNEL (and rct), not user apps.

## Disk image

Unpartitioned FAT16. Do not smash an x86 BPB if present; new `disks/ros.img` is built for rOS. Root contains `IO.SYS`, `KERNEL.SYS`, `COMMAND.COM`, and `HELLO.RXE` once exec exists.

Makefile: `make bios`, `make io`, `make kernel`, `make command`, `make disk`, `make run` → `rvm bios.bin --disk disks/ros.img`. Layer tests: `make test-bios` / `test-io` / `test-kernel` with stub successor files.

## NOP / HALT (prerequisite)

`NOP` is opcode **0x42**. `HALT` is opcode **0x00** (operands ignored). There is no legacy `0x00`+nonzero no-op. Bank padding and `STORC` fill use 0x42. See [isa.md](isa.md).
