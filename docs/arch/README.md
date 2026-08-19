# Ripple architecture

This folder is the **current** architectural specification for the Ripple VM, assembler, and C ABI. If a document under `docs/` disagrees with these pages, these pages win. Implementation details that change often (device bitfields, CLI flags) live next to the code and are linked from here.

| Document | Contents |
|----------|----------|
| [overview.md](overview.md) | 16-bit cells, banks, instruction vs data memory, toolchain |
| [registers.md](registers.md) | 32-register file and symbolic names |
| [isa.md](isa.md) | Encoding, opcodes, CALL/RET, HALT, LOAD/STORE pitfall |
| [abi.md](abi.md) | C data model, calling convention, stack frames |
| [memory.md](memory.md) | Bank map, fat pointers, GEP, heap |
| [mmio.md](mmio.md) | MMIO header summary; full device spec in `rvm/mmio.md` |

Related (not duplicated here):

- Toolchain commands: [`TOOLCHAIN.md`](../../TOOLCHAIN.md)
- MMIO devices: [`rvm/mmio.md`](../../rvm/mmio.md)
- C feature list: [`SUPPORTED_FEATURES.md`](../../SUPPORTED_FEATURES.md)
- Historical drafts: [`docs/archive/`](../archive/README.md)
