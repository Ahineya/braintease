# Instrumented Brainfuck export

`rlink --bf-host` exports the existing BF CPU with explicit host device hooks.
It requires `--format macro --standalone`; ordinary binary and macro exports
keep their existing formats. Guest instructions still execute as Brainfuck.

For an ordinary 16-bit Brainfuck interpreter, omit `--bf-host`. The plain
standalone export retains native `.` output and requires a wrapping tape whose
size is divisible by eight, with enough room for code, RAM and scratch space.
The build helper accepts `BF_HOST=0`; see the [portable run instructions](../../rbf/README.md#ordinary-brainfuck-interpreters).
Instrumented exports delegate TTY output to the host, so ignoring their
annotations alone does not recover that output. Neither export can provide
host peripherals in an interpreter that lacks those peripherals.

```sh
src/ripple-asm/target/release/rasm assemble example.asm -o /tmp/example.pobj
src/ripple-asm/target/release/rlink /tmp/example.pobj \
  --format macro --standalone --bf-host --bank-size 512 --memory-banks 16 \
  -o /tmp/example.bfm
src/bf-macro-expander/target/release/bfm expand /tmp/example.bfm \
  --strip-comments -o /tmp/example.bf
```

The expanded file starts with a self-contained header:

```text
BFHOST 1 BANK_SIZE B MEMORY_BANKS N INSTRUCTIONS I ENTRY E ORIGIN 8 TAPE_CELLS T END_BFHOST
```

All numeric fields are decimal. Header text contains no Brainfuck instructions
or device-hook characters. No sidecar or source map is required. Version 1 uses
16-bit wrapping cell arithmetic and a bounded tape with a zero guard word before
the CPU. `ORIGIN` is the CPU state cell, not the initial tape pointer: execution
starts at cell zero and the exported code advances to cell eight. The guard lets
the original left-seeking macros operate without wrapping the tape pointer.

For origin `O`, the tape layout is:

| Item | Cell index |
| --- | --- |
| CPU state | `O` |
| Register `Rn` | `O + 8*(n+1) + 1` |
| RAM `(bank, offset)` | `O + 8*(bank*B+offset) + 4` |
| Instruction `i` opcode | `O + 8*(40+4*i) + 3` |
| Instruction operands 1, 2, 3 | opcode cell minus 8, 16, 24 |

`N` is the declared RAM limit, not a prediction of every address a program can
compute. Its default is 16. The host must reject accesses outside this limit,
even when the physical tape contains additional cells for code or scratch work.
The exporter reserves the implicit final HALT, ALU scratch words, and a guard:
`T = 8*(max(N*B, 40+4*I+36)+33)`.

The three executable annotations are:

| Marker | Boundary and pointer |
| --- | --- |
| `?` | Before LOAD copies the target RAM cell; pointer is that cell. The host may supply an MMIO read value. |
| `!` | After STORE has written the complete target RAM cell; pointer is that cell. The host may process an MMIO write and update descriptors. |
| `$` | Before each guest instruction, with pointer at its opcode word's lane zero. |

Each LOAD and STORE produces exactly one corresponding hook, including ordinary
RAM accesses. Two identical writes produce two hooks. Optimizations must retain
hook order and pointer position; intermediate BF clear/copy/increment operations
are not independent guest accesses. A NEXT_TICK callback may suspend execution
at `!` and finish the descriptor before resuming. Native TTY output inside the BF
CPU is disabled in this mode: the host outputs bank-zero TTY writes once. BRK is
a no-op, matching normal non-debug BF execution, and produces no extra `$`.

The host uses the existing bank-zero MMIO addresses and device ABI 1. No guest
instruction encodings or C pointer representations change. Raw Brainfuck runners
that ignore these annotations cannot supply the host devices.

Current limits are explicit export errors: multiple code banks, LOADC/STORC,
bank sizes below the 32-cell MMIO header, zero RAM banks, invalid entry points,
and legacy `.data` sections (which overlap MMIO at bank-zero offset 2).
RCC also emits zero-filled `.space` data to reserve global addresses. The linker
recognizes these separately, per object, from matching `__gp_TAG` and
`__rcc_init_TAG` labels. Host export omits these allocation-only bytes; generated
initialization code fills GP bank 1. Mixing in a legacy data object is rejected.
Nonzero entry points within the single code bank are supported. A code bank holds
`B` instructions; each instruction occupies four tape words in the code lane.

The CPU now preserves R0, honors the JAL/JALR link destination (including
non-linking R0 jumps), and compares signed BLT/BGE operands correctly. This is
not an exhaustive ISA parity certification: shift counts outside 0..15 and
division/modulo by zero remain unverified against RVM. Small positive arithmetic
and bitwise operations, normal calls, memory access and branches are covered.

`cargo test --offline --manifest-path src/ripple-asm/Cargo.toml --test bf_host`
expands and executes tiny CPU programs with an independent bounded interpreter.
It checks RAM-bank separation, hook atomicity and repeated writes, host-supplied
LOAD values, strict tape bounds, nonzero entry, backward and signed branches,
JAL/JALR link behavior, R0, arithmetic and bitwise operations, BRK behavior, GP
reservation classification, and rejection of unsupported images. These tests do
not run Doom.
