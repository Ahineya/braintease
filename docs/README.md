# Documentation

## Current architecture

**Start here:** [`arch/README.md`](arch/README.md)

| Topic | Document |
|-------|----------|
| Machine model, banks, toolchain | [arch/overview.md](arch/overview.md) |
| 32-register file | [arch/registers.md](arch/registers.md) |
| ISA and encoding | [arch/isa.md](arch/isa.md) |
| C calling convention | [arch/abi.md](arch/abi.md) |
| Fat pointers and heap | [arch/memory.md](arch/memory.md) |
| MMIO / IRQ | [arch/mmio.md](arch/mmio.md) · [`rvm/mmio.md`](../rvm/mmio.md) |

How to build and run: [`TOOLCHAIN.md`](../TOOLCHAIN.md). C language coverage: [`SUPPORTED_FEATURES.md`](../SUPPORTED_FEATURES.md).

## Still current (non-architecture)

| Document | Topic |
|----------|-------|
| [c-testsuite.md](c-testsuite.md) | Vendored c-testsuite vs Ripple data model |
| [compiler-trace-formats.md](compiler-trace-formats.md) | `rcc --trace` JSON |

## Deprecated

Older specs (18-register ABI, draft ISAs, finished roadmaps) live in [`archive/`](archive/README.md). Stub files remain at the old paths so existing links resolve.
