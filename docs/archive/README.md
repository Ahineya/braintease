# Archived documentation

These files are **not current specifications**. They were written against earlier machines (8/16/18-register drafts, stack-only ABI, pre-MMIO maps, unfinished compiler plans). Keep them for history; do not implement from them.

Current architecture: [`docs/arch/`](../arch/README.md).

## Why each file was archived

| File | What it was | Why it is obsolete |
|------|-------------|--------------------|
| `vm_arch.md` | Early RISC brainstorm | Wrong opcodes, flagless fantasy ISA (`GETPC`, `TRAP`, `HALT=0x3F`), ~8 GPRs, R7 as link register. Never matched RVM. |
| `ASSEMBLY_FORMAT.md` | ISA/system spec (2025-08-07) | Opcodes are close, but bank addressing (`PCB·64+PC·4`), MMIO (only `OUT`/`OUT_FLAG`), and the register file are stale. |
| `ripple-calling-convention.md` | C ABI | 18-register map (R3–R15, R13=SB, R14=SP, R15=FP), stack-only args, no A0–A3 / S0–S3 / GP. |
| `32-REGISTER-UPGRADE.md` | Upgrade **plan** | The 32-register layout shipped. Treat the register table as historical; the live map is [`arch/registers.md`](../arch/registers.md). X0–X3 are no longer “reserved forever” — LOADC/STORC uses them. |
| `more-formalized-register-spilling.md` | Spill algorithm | Allocatable pool R5–R11 and R12 scratch. Allocator now uses T0–T7 + S0–S3 and SC. |
| `calling-convention-analysis.md` | Compiler refactor notes | Paths (`rcc-backend/src/v2/…`) and “stack-only params” are outdated. A0–A3 register args shipped. |
| `v2-backend-architecture.md` | V1→V2 rewrite write-up | Describes 18 regs, R5–R11, `rcc-ir/src/v2/`. The backend lives in `rcc-backend/` with the 32-reg ABI. |
| `rcc-ir-conformance-report.md` | Audit of V1 lowering | Bug report against a backend that was replaced. |
| `c-compiler.prd.md` | Original rcc PRD | 18-reg ABI, no fat pointers in MVP, no float/varargs. Those exist now. |
| `mmio-upgrade.prd.md` | MMIO header PRD | Address map does not match the live header (storage, keyboard, IRQ). |
| `mmio.storage.md` | Storage device spec | Describes 16-bit word disk / 8 GiB. Live device is byte-addressed; see [`rvm/mmio.md`](../../rvm/mmio.md). |
| `about-pointers-and-provenance.md` | Pointer bank lattice | Provenance idea is still valid; register names (GB/R12) and “M3” framing are not. See [`arch/memory.md`](../arch/memory.md). |
| `c-preprocessor.md` | rpp design | Usage is in [`TOOLCHAIN.md`](../../TOOLCHAIN.md) (`rcpp`). This is a feature wishlist, not the implementation. |
| `TYPE_SYSTEM_PREREQUISITES_ROADMAP.md` | Frontend wiring plan (2024–25) | Typedef/symbol work landed; not a spec. |
| `POINTER_ARITHMETIC_ROADMAP.md` | “Frontend must emit GEP” | GEP lowering exists. Bank size is configurable, not fixed 4096. |
| `STDLIB_EXPANSION_PLAN.md` | malloc/libc plan | Runtime has malloc, printf, scanf, softfloat, etc. |
| `FLOATING_POINT_STRATEGY.md` | float representation options | IEEE binary32/binary64 via software is what rcc uses. |
| `COMPILER_FLOAT_SUPPORT.md` | Compiler float implementation plan | Largely implemented (`float`/`double` in the type system). |
| `required_changes.md` | One-line note | “switch to 32 registers” is done. |
| `fix.md` | Scratch list | Not a spec. |

## Stale docs **outside** `docs/` (not moved)

These were not relocated, but they still describe the old machine. Prefer `docs/arch/` over them.

| Path | Issue |
|------|--------|
| `src/ripple-asm/README.md` | Still says 18 registers / R3–R15. Register table updated to point here; instruction list is mostly fine. |
| `rcc-README.md` | “M1 backend skeleton”, old crate layout (`rcc-ir`). Use `TOOLCHAIN.md`. |
| `rcc-backend/src/README_ARCHITECTURE.md` | Mix of current module names and old `rcc_ir` examples. |
| `rcc-backend/src/IMPLEMENTATION_ROADMAP.md` | Completed V2 checklist; still mentions R13 stack-bank init. |
| `public/RIPPLE_ASSEMBLY_TUTORIAL.md` | IDE tutorial with 18-register ABI. |
| `CLAUDE.md` | Agent notes still said 18 registers (updated to point at `docs/arch/`). |
