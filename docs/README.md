# Documentation

## Repository architecture map

The [architecture guide](<repository-map/Braintease architecture guide.docx>) and
[zoomable dependency diagram](repository-map/repository-map.svg) connect the
language frontends, compiler backends, CPU implementations, operating systems,
devices and applications. The map distinguishes implemented paths from prototypes
and later work. A [PNG](repository-map/repository-map.png) and
[editable Graphviz source](repository-map/repository-map.dot) accompany the SVG.

## Current architecture

**Start here:** [`arch/README.md`](arch/README.md)

The [Riplix x64 master plan](x64-master-plan.md) connects the laptop and GUI
vision to boot, applications, input, drivers, execution isolation, native ABIs,
storage and parallel implementation milestones.

| Topic | Document |
|-------|----------|
| Machine model, banks, toolchain | [arch/overview.md](arch/overview.md) |
| 32-register file | [arch/registers.md](arch/registers.md) |
| ISA and encoding | [arch/isa.md](arch/isa.md) |
| C calling convention | [arch/abi.md](arch/abi.md) |
| Native x64 migration plan | [arch/native-x64.md](arch/native-x64.md) |
| Fat pointers and heap | [arch/memory.md](arch/memory.md) |
| Crypto coprocessor | [arch/crypto.md](arch/crypto.md) |
| MMIO / IRQ | [arch/mmio.md](arch/mmio.md) · [`rvm/mmio.md`](../rvm/mmio.md) |

How to build and run: [`TOOLCHAIN.md`](../TOOLCHAIN.md). C language coverage: [`SUPPORTED_FEATURES.md`](../SUPPORTED_FEATURES.md).

## Still current (non-architecture)

| Document | Topic |
|----------|-------|
| [graphics/README.md](graphics/README.md) | Riplix graphics, GX contract, resource budget, Nano-X audit and desktop milestones |
| [graphics/graphite-roadmap.md](graphics/graphite-roadmap.md) | Current FuseDE direction: Graphite components, applications, shared dialogs and desktop services |
| [fusede/README.md](fusede/README.md) | Earlier Nano-X/Slate desktop investigation and existing RVM support |
| [fuse-x/README.md](fuse-x/README.md) | Fuse-X native hosted Riplix direction, desktop integration, storage, networking, and roadmap |
| [rfl/traits-printing.md](rfl/traits-printing.md) | Planned first RFL traits milestone: Printable-based library print/println |
| [rfl/commander-rift-performance.md](rfl/commander-rift-performance.md) | Commander and Rift benchmarks, redraw costs and optimization priorities |
| [rfl/wasm-browser.md](rfl/wasm-browser.md) | Accepted Wasm backend, low-crossing browser runtime, typed UI and parallel implementation plan |
| [rfl/browser-bridge.md](rfl/browser-bridge.md) | Implemented Wasm browser DOM/event bridge, C16 wire ABI, RFL/C examples and lifecycle/crossing checks |
| [rfl/browser-effects.md](rfl/browser-effects.md) | Batched browser timers/fetch, owned RFL futures, mounted async executor, cancellation and wire ABI |
| [rfl/browser-ui.md](rfl/browser-ui.md) | Typed Html/Msg, keyed Wasm renderer, controlled inputs, owned commands and public html! macro |
| [rfl/browser-apps-roadmap.md](rfl/browser-apps-roadmap.md) | Planned request/search app, resource scopes, hash router, storage and classic TodoMVC SPA |
| [rfl/macros.md](rfl/macros.md) | RFL procedural macro syntax/API, hygiene, native bootstrap, limits and JSX-shaped example |
| [rfl/macros-agent-brief.md](rfl/macros-agent-brief.md) | Self-contained implementation task for general RFL procedural macros and JSX-shaped syntax |
| [boot-services.md](boot-services.md) | C service supervisor, Fuse-X boot profiles, once-per-boot mux console, rescue and x64 handoff |
| [terminal-multiplexer.md](terminal-multiplexer.md) | mux: terminal tabs, split panes, scrollback and detachable sessions |
| [JavaScript pipelines](../ports/jpipe/README.md) | Streaming JSON/text adapters, arbitrary map/filter/reduce functions, stable sort and fused QuickJS pipelines |
| [unix-utilities.md](unix-utilities.md) | Installed Unix utility coverage, missing upstream commands and porting priorities |
| [networking/README.md](networking/README.md) | Guest Ethernet, DHCP, UDP/DNS, running examples, tests and TCP/HTTP roadmap |
| [networking/terminal-browser.md](networking/terminal-browser.md) | Terminal browser feasibility, Lynx/Links porting decisions, curses and HTTP milestones |
| [networking/select.md](networking/select.md) | Planned C16 select API, descriptor readiness, kernel wakeups and acceptance criteria |
| [ir.md](ir.md) | Textual IR syntax, tutorials, and lowering rules |
| [self-hosting/README.md](self-hosting/README.md) | Portable C compiler, cproc frontend, C11 direction and Riplix/Fuse-X self-hosting plan |
| [packages/README.md](packages/README.md) | Shared host/guest package tools, binary repositories, `/usr/bin` ownership and Mac mini deployment |
| [c-testsuite.md](c-testsuite.md) | Vendored c-testsuite vs Ripple data model |
| [compiler-trace-formats.md](compiler-trace-formats.md) | `rcc --trace` JSON |
| [compiler-performance.md](compiler-performance.md) | RCC profiling results, preprocessor prototype and parallel compilation plan |
| [compiler-bit-fields.md](compiler-bit-fields.md) | Shared native/Ripple packed bit-field layout and validation |
| [Brainfuck host](../rbf/README.md) | Native BF CPU host, devices, capture and bounded tests |

## Deprecated

Older specs (18-register ABI, draft ISAs, finished roadmaps) live in [`archive/`](archive/README.md). Stub files remain at the old paths so existing links resolve.
