# C ABI

This is the calling convention the compiler (`rcc-backend`) and `crt0` actually use. Source: `rcc-backend/src/function/calling_convention.rs`, `rcc-backend/src/function/internal.rs`, `runtime/crt0.asm`.

## Data model (16-bit C bytes)

A C byte is **one 16-bit addressable VM cell** (`CHAR_BIT == 16`). `sizeof`,
character-pointer arithmetic, object layout, and the size arguments of allocation
and memory-copy functions all use this same unit. This preserves the VM ISA and
its Brainfuck implementation; the compiler does not pack C characters into octets.

| Type | `sizeof` (C bytes / cells) | Value bits |
|------|---------------------------|------------|
| `_Bool` | 1 | 1 |
| `char`, signed/unsigned char, `short`, `int`, `enum` | 1 | 16 |
| `long`, `float`, pointer, `size_t`, `ptrdiff_t` | 2 | 32 |
| `long long`, `double` | 4 | 64 |

Plain `char` is signed. Unsigned char and unsigned short promote to unsigned int,
because their full range cannot be represented by signed int. `size_t` is unsigned
long and `ptrdiff_t` is long, permitting sizes and differences across banks.
Pointers are **fat**: address word + bank word. Multi-cell integers and floats are
little-endian (low word first).

`uintptr_t` encodes a pointer as `(bank << 16) | offset`; null converts to zero.
Pointer equality and truth tests consider both words. Pointer arithmetic normalizes
offsets at `BANK_SIZE`, and subtraction uses the linear cell address
`bank * BANK_SIZE + offset`, returning a signed `ptrdiff_t`. Multi-cell loads and
stores also advance to the next bank at the boundary. These operations use the
existing VM instructions.

There is no exact-width `int8_t` or `uint8_t`: `<stdint.h>` provides
`int_least8_t` and `uint_least8_t` instead, both 16 bits. File formats such as WAD
and FAT still contain 8-bit octets. Their adapters must extract/mask octets and
decode fields explicitly; an on-disk octet count is not an in-memory C structure
size. Code relying on `CHAR_BIT == 8` requires porting.

The lexer currently accepts numeric character escapes only through 255 and
reports an explicit unsupported-feature diagnostic above that range. Full-cell
values can be expressed numerically, for example `(unsigned char)0x1234U`.

All code and runtime objects must be rebuilt together after this ABI change.

## Symbols, initialization, and callbacks

Each global definition receives a linker-visible data label. `extern` declarations
refer to that label without reserving another object. Repeated declarations in one
translation unit share a definition; static objects, static functions, and string
literal labels are renamed per object to preserve internal linkage.

Each translation unit with defined global objects emits a `__rcc_init_*` routine.
The linker constructs `_init_globals`, which saves RA/RAB using SB/SP and calls
all these routines before `crt0` calls `main`. Scalar, pointer, and aggregate
initializers are emitted at their declared cell widths. Global data currently
occupies one GP bank: the compiler checks each unit and the linker checks the
combined allocation, reporting overflow instead of wrapping into stack storage.
Use assembler `--memory-offset 0` for compiler-generated objects, as the runtime
build and C test scripts do.

Function pointers use two cells: the in-bank instruction offset, followed by the
code bank. They use the same argument and return slots as data pointers. Indirect
calls execute the existing `JALR RA, bank_register, offset_register` instruction.
The assembler's `%offset(function)` and `%bank(function)` expressions remain
relocatable until the linker has packed the functions into code banks. Code-bank
`LI` operands reuse the existing RXE `RELOC_JAL_ABS` relocation of instruction
word2, so relocating callbacks requires no new loader format or VM instructions.
Global callback variables, arrays, and struct tables can therefore refer to
functions in another translation unit or code bank.

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
| Fat pointer / aggregate-result address | RV0 = address, RV1 = concrete bank |
| 32-bit (`long` / `float`) | RV0 = low, RV1 = high |
| 64-bit (`long long` / `double`) | RV0, RV1, X0, X1 (low to high) |

## Clobbers

- **Caller-saved:** T0–T7, A0–A3, RV0–RV1 (and X* when used as wide scratch).
- **Callee-saved:** S0–S3. The compiler currently **always** saves/restores all four in every function prologue/epilogue.
- **RA / RAB:** clobbered by `CALL`/`JAL`/`JALR` while IRQ entry preserves them. Callees save both.

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
| above locals | the function's complete spill area (at least 20 cells), reserved before any call |

Stack arguments sit **below** the saved RA. First stack argument is at `FP-8` (and lower for later/wider args). `calling_convention.rs` starts at `-7` for the saved block and subtracts slot counts for each stack parameter.

The compiler checks local frame sizes before narrowing FP offsets, and checks the
final frame against the stack bank. Each call uses the final spill extent, so
spills emitted after a call cannot overwrite an aggregate returned from the
callee's frame before it is copied. Recursive call depth is still limited by the
single stack bank.

Epilogue restores S3…S0, then FP, RAB, RA, then `RET`.

## Interrupts vs the ABI

IRQ ABI version 2 saves its continuation separately and preserves RA/RAB. Assembly
handlers save all other live registers they modify, acknowledge the active event,
and use IRET after restoring registers. ACK keeps dispatch inhibited until the
atomic return. Optional interrupt-stack switching and editable return state are
specified in [stacks-and-interrupts.md](stacks-and-interrupts.md).

## Optional growing stacks

`--segmented-stack` adds entry checks and bank transitions using a per-stack pool.
Escaping stack pointers carry concrete banks, and aggregate returns carry both
pointer words. Fixed stacks remain the default. See
[stacks-and-interrupts.md](stacks-and-interrupts.md) for setup, assembly interop,
bootstrap requirements, and migration.
