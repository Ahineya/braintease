# Register file

The VM has **32** 16-bit registers. The assembler accepts numeric names (`R0`…`R31`) and symbolic names. Both are case-insensitive.

Source of truth: `src/ripple-asm/src/types.rs` (`enum Register`).

| Numeric | Symbolic | Role |
|---------|----------|------|
| R0 | R0 / ZR | Hardware zero (reads 0; writes ignored) |
| R1 | PC | Program counter (offset within IMEM bank) |
| R2 | PCB | Program counter bank |
| R3 | RA | Return address (offset) |
| R4 | RAB | Return address bank |
| R5 | RV0 | Return value 0 (scalar, or fat-pointer address) |
| R6 | RV1 | Return value 1 (second word, or fat-pointer bank) |
| R7 | A0 | Argument 0 |
| R8 | A1 | Argument 1 |
| R9 | A2 | Argument 2 |
| R10 | A3 | Argument 3 |
| R11 | X0 | Instruction-memory copy cell 0 (opcode); also 64-bit/softfloat scratch |
| R12 | X1 | IMEM copy cell 1 |
| R13 | X2 | IMEM copy cell 2 |
| R14 | X3 | IMEM copy cell 3 |
| R15–R22 | T0–T7 | Temporaries (caller-saved, allocatable) |
| R23–R26 | S0–S3 | Callee-saved (allocatable) |
| R27 | SC | Allocator scratch (spill address calc; compiler-owned) |
| R28 | SB | Stack bank id |
| R29 | SP | Stack pointer (data address within `SB`) |
| R30 | FP | Frame pointer |
| R31 | GP | Global bank id |

## Classes (C compiler)

| Class | Registers | Notes |
|-------|-----------|--------|
| Hardware | R0, PC, PCB, RA, RAB | Not allocated. `JAL`/`JALR` and IRQs write RA/RAB. |
| Return | RV0, RV1 | See [abi.md](abi.md). |
| Arguments | A0–A3 | First ABI slots of a call. Clobbered by calls. |
| Allocatable | S0–S3, T0–T7 | 12 registers. Allocator prefers S* then T*. |
| Scratch / ABI special | SC, SB, SP, FP, GP | Not in the allocatable pool. |
| X0–X3 | | Not allocated for ordinary temps. `LOADC`/`STORC` fill/spill them. Softfloat / `long long` lowering also uses them as wide scratch. |

`crt0` sets `SB=2`, `SP=1`, `FP=1`, `GP=1` before calling `main`.
