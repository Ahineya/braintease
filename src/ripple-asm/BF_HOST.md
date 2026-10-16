# Instrumented Brainfuck export

`rlink --bf-host` exports the existing BF CPU with explicit host device hooks.
It requires `--standalone` with `--format macro` or `--format bf`; ordinary
binary and macro exports keep their existing formats. Guest instructions still
execute as Brainfuck.

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

For large programs, export expanded BF directly:

```sh
src/ripple-asm/target/release/rlink /tmp/example.pobj \
  --format bf --standalone --bf-host --bank-size 512 --memory-banks 16 \
  -o /tmp/example.bf
```

This path streams the code initializer, using short ordinary BF loops for large
constants and a bounded scan to clear setup markers. It avoids constructing a
macro syntax tree for millions of instruction words or emitting tens of
thousands of `+` characters per constant. The CPU executor is expanded from the
same CPU templates and generated portable CPU routines. Initialization preserves device RAM, restores scratch
cells and reaches the same tape state at the first instruction hook. Output is
written to a temporary sibling and renamed only after successful completion.
Direct BF output currently requires host mode; plain exports still use the
macro path. Neither path adds native guest-instruction execution to the host.

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

Instruction index `i` is `PCB*B+PC`; the code lane stores all banks consecutively.
`ENTRY` is an absolute instruction index, so startup sets `PC=ENTRY%B` and
`PCB=ENTRY/B` while positioning the tape at the complete entry index. At `$`, the
pointer is `O+8*(40+4*(PCB*B+PC))`. The host validates this position and the image
bounds; it does not choose or execute the next guest instruction.

`N` is the declared RAM limit, not a prediction of every address a program can
compute. Its default is 16. The host must reject accesses outside this limit,
even when the physical tape contains additional cells for code or scratch work.
The exporter reserves the implicit final HALT, ALU scratch words, and a guard:
`T = 8*(max(N*B, 40+4*I+36, 224)+33)`.
The 224-word minimum accommodates persistent CPU state in unused register-lane
cells; it does not move architectural registers, instructions or guest RAM.

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

Current host limits are explicit export errors: IRQ instructions without
`--bf-irq`, bank sizes below the 32-cell MMIO header, zero RAM banks, invalid entry points,
and legacy `.data` sections (which overlap MMIO at bank-zero offset 2).
RCC also emits zero-filled `.space` data to reserve global addresses. The linker
recognizes these separately, per object, from matching `__gp_TAG` and
`__rcc_init_TAG` labels. Host export omits these allocation-only bytes; generated
initialization code fills GP bank 1. Mixing in a legacy data object is rejected.
Both plain and host exports support multiple code banks and nonzero entry points
in any bank. A code bank holds `B` instructions; each instruction occupies four
tape words in the code lane. Bank seeks use nested Brainfuck loops, avoiding a
16-bit intermediate product for the full tape address. The image plus its
implicit final HALT must fit the 16-bit PC/PCB address space (`I < B*65536`).

Control flow preserves the Rust VM's existing register semantics. Calls save
the raw `(PC+1, PCB)` link pair; jumps and taken branches can leave `PC>=B`.
Fetch still uses `PCB*B+PC`. An ordinary advance increments PC when `PC+1<B`,
otherwise resets PC to zero and increments PCB. In particular, returning from a
call in the last slot of a bank gives raw `PC=B` in the original bank; the next
ordinary advance normalizes the pair. The tests retain this edge behavior
rather than changing the ISA. Code-memory access and opt-in IRQ ABI 2 are
implemented as ordinary Brainfuck, as described below.

The CPU now preserves R0, honors the JAL/JALR link destination (including
non-linking R0 jumps), and compares signed BLT/BGE operands correctly. This is
not an exhaustive ISA parity certification. Shifts now mask counts with 15,
and unsigned DIV/MOD return zero for a zero divisor, matching RVM. Arithmetic
coverage includes high-bit operands, large divisors, register/immediate shifts,
source/destination aliases and scratch preservation.
General ALU/LOAD writes directly to PC/PCB still do not reliably reposition the
physical BF instruction pointer; the bank support covers branches, JAL/JALR,
startup and sequential advance.

`cargo test --offline --manifest-path src/ripple-asm/Cargo.toml`
expands and executes tiny CPU programs with an independent bounded interpreter.
It checks RAM-bank separation, hook atomicity and repeated writes, host-supplied
LOAD values, strict tape bounds, nonzero entry, backward and signed branches,
JAL/JALR link behavior, R0, arithmetic and bitwise operations, BRK behavior, GP
reservation classification, and rejection of unsupported images. The multi-bank
tests cover non-power-of-two banks, later-bank entry, nested calls, aliased JALR
operands, raw boundary return addresses, maximum-PC rollover, branches at bank
edges, and sequential fallthrough. Plain-mode runs check the same CPU without
host hooks, using 16-bit wrapping cells and a circular tape. These tests do not
run Doom.

`cargo test --offline --manifest-path src/ripple-asm/Cargo.toml --test bf_compact`
compares direct and macro exports using an independent BF interpreter: complete
initial and final tapes, hooks, output, entry points, cross-bank calls and
prepopulated device memory. It also checks large-image setup cleanup and export
validation.

## Code memory and optional interrupts

`LOADC R0, bank_reg, offset_reg` reads an instruction into X0..X3 (opcode,
operand 1, operand 2, operand 3). `STORC` writes those four fields, masking X0 to
its low octet. Both snapshot the address before accessing any X register, keep
RAM separate from code, and support self-modification and later code banks.
The next fetch observes the new instruction in both BF interpreter and JIT:
there is no native Ripple instruction cache to invalidate.

STORC can grow the loaded image. Gaps become NOP instructions; growth rounds
up to the destination bank's end where capacity permits. New cells are initialized
in one physical pass, avoiding a quadratic sequence of code seeks. Growth is bounded by
the declared physical tape. By default it also observes RVM's
4,194,304-instruction STORC limit. With
`W=max(N*B,76+4*I,224)`, physical code capacity is `floor((W-76)/4)+1`
instructions, additionally bounded by the 16-bit PC/PCB space. Increasing
`--memory-banks` reserves more tape and therefore more room for dynamic code;
code and RAM occupy different lanes and do not overwrite each other. Ordinary
plain exports use sixteen banks for this capacity calculation; their interpreter
must supply enough tape for that reservation before using dynamic code growth.
LOADC rejects addresses outside the loaded extent. The implicit final HALT is a
fetch sentinel, not an extra instruction exposed by LOADC; extension replaces
it with NOP along with other newly loaded cells.

For disk loaders, `--code-banks C` explicitly reserves at least `C*B`
instructions without increasing guest RAM. This requires host standalone macro
or BF output. The physical allocation becomes
`T = 8*(max(N*B, 76+4*I, 76+4*C*B, 224)+33)`; the BF host still enforces
`N` RAM banks. An explicit reservation can exceed the default STORC limit,
within the architectural 16-bit PC/PCB space. FUZIX uses 68 code banks and
80 RAM banks, loading the BIOS, kernel and userspace into separate code ranges.

Interrupts are **compiled out by default**. Enable them when linking:

```sh
rlink example.pobj -f bf --standalone --bf-host --bf-irq \
  --bank-size 512 --memory-banks 16 -o example.bf
```

The build helper accepts `BF_IRQ=1 scripts/build-bf.sh source.asm`.

For a macro export, use the same `--bf-irq` flag, or change
`#define ENABLE_IRQ 0` to `#define ENABLE_IRQ 1` in the generated standalone
`.bfm` before expansion. The expander's conditional removes boundary polling,
timer accounting and IRQ STORE handling completely when disabled. There is no
per-instruction runtime IRQ-enable test in that CPU variant. Exporting IRET,
CSRR or CSRW instructions in host mode requires the explicit flag.

The enabled CPU implements [IRQ ABI 2](../../docs/arch/stacks-and-interrupts.md):
all sixteen pending/enable bits, lowest-bit priority, non-nesting BUSY,
acknowledgement separate from return, reassertion, editable saved PC/PCB,
enable and stack state, optional interrupt-stack selection, IRET and all twelve
CSRs. Its 32-bit timer counts guest instructions, including handler work;
writing either timer word restarts it and excludes that configuration instruction.
It is independent of device NEXT_TICK, wall time and the BF engine's dispatch count.
Checks occur before each guest instruction, exactly as in RVM.

IRQ MMIO belongs to the CPU. Host device callbacks preserve its shadow instead
of simulating interrupts. Internal state lives in the unused register lane after
the architectural registers and is automatically included in tape checkpoints.
The exporter emits these additional routines from `src/bf_cpu/`; the existing
header/footer arrange dispatch, scratch capture and physical PC positioning.
Only generation happens in Rust; the resulting routines contain ordinary BF.

Invalid CSR operands, trap-state/interrupt-stack operations and invalid code accesses stop the
CPU and publish an error code in register-lane word 57. RBF reports this as an
execution failure. Word 80 identifies the new CPU state layout; words 81–82
publish its loaded code extent, with word 87 marking its exclusive full-space
end. These are CPU internals, not MMIO or guest-addressable CSRs. The host only
validates the published extent and instruction-hook position.

`bf_system` runs both plain and hosted macro exports in an independent bounded
16-bit BF evaluator. It covers aliases, all four instruction fields, high values,
self/next-instruction modification, new code, capacity faults, non-power-of-two
banks, IRQ priority, acknowledgement/reassertion, frame editing, stack switching,
CSR failures, timer timing/borrowing/HALT behavior and exact compile-time IRQ
removal. A large-bank case crosses the 65,535-word tape boundary while checking
code fields, RAM preservation and scratch cleanup. The existing RVM assembly
examples also run unchanged in both plain and hosted BF.
`cargo test -p rbf --test cpu_system` additionally checks compact-export execution
in the real BF interpreter and JIT against RVM registers and memory, and checks
runtime fault reporting. No Doom run is needed.


## Arithmetic and tape paths

The [BF CPU performance notes](../../docs/arch/bf-cpu-performance.md) describe
local bit extraction, binary long division, whole-value register/RAM transfers,
and the generic guarded BF optimizations used by the native interpreter/JIT.
These change neither the instruction set nor the requirement for 16-bit cells.
