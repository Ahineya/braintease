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
| [rfl/traits-printing.md](rfl/traits-printing.md) | Planned first RFL traits milestone: Printable-based library print/println |
| [networking/README.md](networking/README.md) | Guest Ethernet, DHCP, UDP/DNS, running examples, tests and TCP/HTTP roadmap |
| [ir.md](ir.md) | Textual IR syntax, tutorials, and lowering rules |
| [c-testsuite.md](c-testsuite.md) | Vendored c-testsuite vs Ripple data model |
| [compiler-trace-formats.md](compiler-trace-formats.md) | `rcc --trace` JSON |
| [Brainfuck host](../rbf/README.md) | Native BF CPU host, devices, capture and bounded tests |

## Deprecated

Older specs (18-register ABI, draft ISAs, finished roadmaps) live in [`archive/`](archive/README.md). Stub files remain at the old paths so existing links resolve.
