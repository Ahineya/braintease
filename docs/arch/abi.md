# C ABI

This is the calling convention the compiler (`rcc-backend`) and `crt0` actually use. Source: `rcc-backend/src/function/calling_convention.rs`, `rcc-backend/src/function/internal.rs`, `runtime/crt0.asm`.

## Data model (ILP16-ish)

C `sizeof` is in **bytes**. Each VM cell is 16 bits, so `char` still occupies one cell.

| Type | `sizeof` (bytes) | Cells |
|------|------------------|-------|
| `_Bool`, `char`, signed/unsigned char | 1 | 1 |
| `short`, `int` (signed/unsigned) | 2 | 1 |
| `long`, `float`, pointer | 4 | 2 |
| `long long`, `double` | 8 | 4 |
| `enum` | 2 | 1 |

Pointers are **fat**: address word + bank word. Multi-cell integers and floats are little-endian (low word first).

## Register arguments

Arguments occupy **slots** in A0–A3 (4 slots):

| Value | Slots |
|-------|-------|
| 16-bit scalar | 1 |
| Fat pointer, `long`, `float` | 2 |
| `long long`, `double` | 4 |

Fill A0–A3 in order. If the next argument does not fit in the remaining slots, **that argument and every later one** go on the stack (a fat pointer is never split across registers and stack). Variadic extra arguments are always on the stack.

## Returns

| Result | Registers |
|--------|-----------|
| 16-bit scalar | RV0 |
| Fat pointer | RV0 = address, RV1 = bank |
| 32-bit (`long` / `float`) | RV0 = low, RV1 = high |
| 64-bit (`long long` / `double`) | RV0, RV1, X0, X1 (low to high) |

## Clobbers

- **Caller-saved:** T0–T7, A0–A3, RV0–RV1 (and X* when used as wide scratch).
- **Callee-saved:** S0–S3. The compiler currently **always** saves/restores all four in every function prologue/epilogue.
- **RA / RAB:** clobbered by `CALL`/`JAL`/`JALR` and by IRQ dispatch. Callees save both.

## Stack

Grows **upward**. Bank is `SB` (crt0: 2). `SP` and `FP` start at 1.

Compiler frame (low → high), then `FP` is set to current `SP`:

```
incoming stack args (below the frame; see offsets)
...
[RA] [RAB] [old FP] [S0] [S1] [S2] [S3] [locals …] [spill reserve]
 ^saved at SP before FP is updated              ^ FP points here
```

After prologue:

| Location | Contents |
|----------|----------|
| FP-7 | saved RA |
| FP-6 | saved RAB |
| FP-5 | saved FP |
| FP-4 … FP-1 | S0 … S3 |
| FP+0 … | locals |
| above locals | ~20 reserved spill slots; SP is raised further around calls if needed |

Stack arguments sit **below** the saved RA. First stack argument is at `FP-8` (and lower for later/wider args). `calling_convention.rs` starts at `-7` for the saved block and subtracts slot counts for each stack parameter.

Epilogue restores S3…S0, then FP, RAB, RA, then `RET`.

## Interrupts vs the ABI

IRQ dispatch writes RA/RAB like `JAL` but saves the **current** PC (the instruction has not executed yet). Handlers must `ACK` (`IRQ_BUSY` write nonzero) before returning, or keep ENABLE clear, so a still-pending IRQ cannot overwrite RA/RAB. Same-bank: `RET`. Cross-bank: `JALR R0, RAB, RA`. No nesting while `IRQ_BUSY` is set. Details: [mmio.md](mmio.md).
