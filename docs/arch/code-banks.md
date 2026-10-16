# Functions across code banks

One C or RFL function may occupy several instruction-memory banks. This is
automatic in the shared assembler/linker; no source annotation, compiler mode,
new opcode or VM change is required. Native compilation has no Ripple code-bank
placement requirement.

Keep the same `--bank-size` throughout the Ripple toolchain. Small functions
prefer their existing placement within one bank. Larger functions continue
through subsequent banks, using the CPU's ordinary sequential fallthrough.
Instruction and data addressing remain separate.

## Branches and calls

Conditional branches update PC without updating PCB. The assembler therefore
retains symbolic branch and jump destinations in object relocations, including
local destinations and displacements beyond 65,535 instructions. The linker
chooses encodings after placement:

- A branch within one bank keeps its ordinary one-instruction encoding.
- An unconditional branch across banks becomes `JAL R0, bank, offset`.
- A conditional branch across banks becomes its inverse comparison followed
  by a non-linking far jump. For example, a cross-bank `BEQ A0, A1, target`
  becomes:

  ```asm
  BNE A0, A1, 2
  JAL R0, target_bank, target_offset
  ; ordinary fallthrough continues here
  ```

The linker keeps that short skip and its continuation in the same bank, adding
NOP padding when necessary. `JAL R0` leaves RA/RAB intact. Equal-operand
BEQ/BGE use a single far jump; equal-operand BNE/BLT never take their branch.

Code growth can move other branches across banks. Placement repeats until no
new far branches are needed; promoted branches never shrink during this process,
so it terminates without layout oscillation. Labels, function-pointer bank/offset
components, entry points, debug addresses and executable relocations use the final
instruction positions. Generated far jumps carry the existing JalAbs relocation,
including in RXE1 images. SYS1/RLINK/BF outputs share the same placement.

Calls in spanning or conditionally expanded function segments are kept away
from the final instruction of a bank. The existing CPU saves raw PC+1 on calls;
a return PC equal to bank_size can alias the next bank's first instruction and
execute it twice. Padding before such a call gives it a canonical continuation.
This applies to direct JAL and indirect JALR calls. The ISA's raw return behavior
and deliberately positioned low-level assembly remain unchanged.

## Assembly and compatibility

Use symbolic destinations (`BNE A0, R0, L_loop`) for relocatable code. Numeric
branch displacements retain their low-level ISA meaning. If expansion or packing
would change the distance represented by a numeric branch, linking fails with
an explicit request to use a symbolic label. Reassemble older objects whose
resolved numeric branches cannot survive the new placement. Numeric JAL bank
operands are not mistaken for relocatable local labels.

Normal `.pobj` compilation and `rlink` need no new flags. `rasm assemble --format
macro` and `rasm format --bank-size N` now link before exporting BF macros.
The WASM source-to-macro helper also uses the shared linker. Undefined symbols
are errors in executable exports, rather than zero-filled branch placeholders.
Raw RASM binary assembly reports that linking is required when symbolic control
flow needs bank-aware placement; use the normal object/link pipeline.

The Rust `format_to_macro` convenience helper now returns
`Result<String, Vec<String>>` and links using the default bank size. Use
`RippleAssembler::assemble_to_macro` with explicit assembler options, or
`Linker` plus `MacroFormatter`, for another bank size.

This removes the per-function code-bank limit. Existing total code-address,
object-format, data-bank and stack-frame limits still produce errors. A far
conditional sequence needs a bank size of at least three instructions; normal
runtime banks are substantially larger.

## Regression checks

- `src/ripple-asm/tests/code_banks.rs`: wide symbolic targets, repeated layout
  relaxation, short-branch encoding, constant comparisons, external targets,
  reachability collection, executable exports and explicit malformed/numeric
  relocation errors.
- `rvm/tests/code_banks.rs`: all comparison forms and both outcomes at every
  alignment in 3-, 8- and 37-instruction banks; direct/indirect boundary calls,
  65,535-instruction banks, a 70,000-instruction function, relocated function
  pointers and a cross-bank hot loop with compiled JIT regions.
- `src/ripple-asm/tests/bf_multibank.rs`: the linked spanning-function fixture
  executes on both the plain and host BF CPUs using an independent BF evaluator.
- `c-test/tests/bugs/test_function_spans_code_banks.c`: a compact macro generates
  65,536 NOPs inside one C function, exercising an early-return branch and normal
  fallthrough through the ordinary C toolchain. Expected output is `YY`.

```sh
CARGO_PROFILE_RELEASE_PANIC=unwind cargo test --release --manifest-path src/ripple-asm/Cargo.toml --tests
cargo test -p rvm --test code_banks
./rct test_function_spans_code_banks --timeout 20
```

The panic override lets optimized test harnesses unwind instead of inheriting
the standalone package's abort setting. Ordinary debug tests also work.

Validation on 16 October 2026: **160 assembler/BF tests**, **329 RFL release
tests**, the **five RVM bank tests**, and **five C regressions** passed. The
spanning-function fixture also printed `Y` through the actual RBF launcher in
both interpreter and JIT modes. Direct source-to-macro and object-to-macro CLI
exports matched. The WASM feature passed compilation checks.

As an additional integration check, the task-scope example was compiled with
the saved RFL compiler from before cleanup-helper sharing. Its main contains
73,955 assembly instructions and previously could not link with 64,000-instruction
banks. With the updated assembler/linker it ran on RVM and printed all six
expected greeting, result and cancellation lines. This validates the larger
function independently of the separate cleanup-size optimization.
