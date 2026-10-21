# Native x64 Riplix migration

Status: accepted direction, 2026-10-20. Implementation proceeds through the
acceptance gates below. Existing x64 and Fuse-X packages still use the C16
compatibility ABI; this decision does not enable a new executable format,
change defaults or establish native application support.

The [x64 master plan](../x64-master-plan.md) places these execution stages
alongside laptop drivers, applications, graphics, storage and boot delivery.
The contracts and execution acceptance gates below remain authoritative.

The x64 destination is conventional byte-addressed applications and, later, a
native kernel build. Keep Ripple/C16 useful and retain compatibility applications
during migration. Share kernel, library and application sources wherever their
semantics permit; maintain target definitions and platform adapters rather than
forking the operating system into two independent implementations.

Graphics precision is independent of the execution ABI. Future native graphics
should negotiate surface formats through the [GX format contract](../graphics/architecture.md#gx-version-1),
prefer full eight-bit RGB channels where supported, and retain RGB565 for
compatibility clients or constrained backends. The hosted Fuse-X full-colour
implementation establishes that contract; it does not establish an x64 graphics
driver or make C16 pointers valid in native framebuffer code.

## Current execution model

RCC emits x64 machine instructions while retaining 16-bit C characters and
integers, logical bank/offset pointers, cell-based aggregates and function
tokens. Native functions use a private runtime-context/scalar-carrier calling
convention. Application suspension preserves compiler-generated continuation
frames; it does not preserve an arbitrary native call stack.

The x64 platform already has native hardware addresses, physical-page
allocation, page-table setup, interrupt entry and device drivers. Its current
application memory model and scheduler integration remain distinct from
conventional x64 processes. See the [x64 implementation and acceptance
notes](../../tools/x86_64/README.md), [target layouts](../../rcc-ir/src/target.rs),
[native lowering](../../rcc-native/src/lib.rs) and
[process snapshots](../../tools/x86_64/boot/process.inc).

## Separate architectural decisions

| Change | Benefit | Does not establish |
| --- | --- | --- |
| Native byte addressing and ABI | Direct memory operations, ordinary pointers, broader compiler and library compatibility | Preemption or multicore execution |
| Preemptive application scheduling | Fairness and responsiveness without compiler checkpoints | Faster computation or a new C data model |
| SMP | Concurrent execution on multiple cores | Safe concurrent access to existing global kernel/runtime state |
| Native kernel build | Fewer representation boundaries and conventional kernel tooling | Linux binary compatibility or unchanged disk layouts |

C16 is a valid C data model. Its principal costs here are translated memory
accesses, unusual library assumptions, continuation machinery and current logical
address-space limits. Machine-code generation already uses native registers and
instructions. Wider integers alone are not the performance goal. Native pointers
and aligned structures can consume more space; octet buffers no longer need one
16-bit cell per byte. Benchmark execution time, memory, startup and code size
separately before claiming gains.

A C16 kernel can initially manage native allocations through platform handles and
adapters. Its own logical address range need not contain all application RAM or
hardware memory. Moving the kernel to native C is the eventual simplification,
not a prerequisite for the first native application or for preemption.

## Application and kernel boundaries

```text
C16 application -> compatibility memory and continuation runtime --+
                                                                |
Native application -> native libc and syscall adapter ------------+
                                                                |
                                      one Riplix kernel and scheduler
                                      files, pipes, processes, devices
```

Each process has an explicit execution ABI. The existing kernel remains the
authority for PIDs, scheduling, descriptors, permissions and process lifetime.
The runtime restores either compatibility continuations or native execution
state according to that tag; it does not introduce a second run queue or OS.

The native userspace baseline is System V AMD64 with LP64: 8-bit `char`, 16-bit
`short`, 32-bit `int`, 64-bit `long`, pointers and `size_t`, and normal alignment,
aggregate and variadic conventions. Start with statically linked ELF64 images.
Define an explicit, versioned Riplix executable/syscall contract; ELF machine
identity alone cannot distinguish incompatible application ABIs. Linux and
macOS binaries are not implicitly supported.

The syscall boundary must translate user buffers, strings, argument arrays,
public structures and errors. Native pointers must never be passed as logical
C16 pointers, and logical pointers must never become hardware DMA addresses.
Validate complete ranges and integer conversions. Per-process transfer storage
must survive a blocking operation and remain private until completion or cancel.

Native file octets need a documented mapping onto existing C16 file contents,
including treatment of cells above 255. Define this before binary I/O acceptance.
Preserve the existing disk encoding; never serialize a native `struct` in its
place. Native integer widths do not automatically lift filesystem sizes, PID
counts or other service limits. Return a defined overflow/unsupported error for
requests beyond the implemented kernel contract.

## Execution and scheduling requirements

Native processes need virtual-memory mappings, ordinary stacks, saved CPU state
and kernel entry/return. Introduce separate page tables and ring-3 execution
before accepting substantial native applications. Account for user and kernel
stacks, floating-point state, executable permissions, guard pages, checked user
copies and per-process exception handling. Failed loading or allocation must
leave the caller intact and release staged resources.

Keep one CPU initially. Preempt native application execution from timer events
without requiring compiler-inserted checkpoints. The kernel may initially remain
nonpreemptible, with switching deferred until its state and runtime operations
are safe. Bound long protected operations; otherwise application preemption alone
does not prevent visible stalls.

Arbitrary interruption of the compatibility runtime is separate work. Switching
the active C16 backing while a helper retains a borrowed pointer can invalidate
that pointer. Audit stack, memory-view and temporary-storage lifetimes, then
either preserve those invariants across switching or explicitly defer switching
inside those regions. Do not convert the current timer ISR into an unconditional
context switch. C16 preemption does not require changing C16 language semantics.

Native `fork` preserves virtual addresses and private writable state. Begin with
eager copying, then add hardware copy-on-write with page-fault and ownership
tests. Copying native memory to different addresses without preserving mappings
does not preserve arbitrary pointers. `exec` must transition between both ABI
families, stage a replacement before commit and retain normal descriptor rules.
Signal handling needs defined native delivery/return semantics and must preserve
interrupted state; unsupported handler behavior must not silently succeed.

SMP follows working single-CPU scheduling and memory isolation. It requires
per-CPU contexts, synchronization of shared kernel/runtime state, address-space
coordination and device concurrency rules. A coarse kernel lock can be an early
step; more cores cannot safely be enabled by changing the C data model alone.

## Implementation stages and acceptance

### C16 process execution priority

The immediate priority is cheaper hardware-backed process switching and
preemptible C16 applications. Keep their current language layouts and private
calling conventions initially; LP64 is not a prerequisite for ring-3 execution.
First measure scheduling gaps and snapshot-copy overhead, bound long kernel
operations, then introduce mapped process backing while retaining safe
checkpoint scheduling. Separate private user runtime state from privileged
services before adding ring-3 entry and timer-driven machine-context switching.

This narrows the first execution milestone below to the existing C16 application
family. Standard ELF/LP64 applications remain the later native target. Loader
permissions and helper/trap contracts still need explicit design, and existing
packages are not automatically safe to run at ring 3. Coordinate source and
platform-interface ownership through the
[shared workstream note](native-memory-coordination.txt); the x64 owner's
driver, DMA, boot and hardware-acceptance work remains independent until an
integration change is explicitly agreed.

The first delegated slice now has an isolated
[process-memory model and host contract](../../tools/x86_64/process-memory/DESIGN.md):
caller-owned metadata, callback-backed sparse 4 KiB payloads, transactional
eager cloning and mapping-plan queries. Its host tests do not activate page
tables or change scheduling. Production integration remains a separate x64
owner review; no kernel, executable or read-view ABI changes follow from the
prototype's validation.

### Full native migration stages

1. **Freeze the native contracts.** Specify ELF acceptance, entry stack,
   syscall invocation and errors, type layouts, user address ranges, ABI tags,
   file-byte semantics and process ownership. Define supported relocations and
   reject others. Keep dynamic loading and native shared libraries out of the
   first milestone. Add layout/call probes for pointers, structures,
   floating-point arguments, callbacks and varargs.
2. **Establish native execution.** Use a host Clang cross-build, a native startup
   object and a bounded test image. Add mapped user code/data/heap/stack,
   privilege transitions and saved machine contexts. Acceptance: native
   calculation, output through the defined syscall entry and clean exit;
   malformed images, invalid user memory and application faults leave the
   existing shell and kernel running. Exercise native allocations beyond the
   compatibility heap limit within an explicit physical-memory budget.
   Repeated launch/exit must reclaim pages.
3. **Integrate kernel services.** Implement native syscall marshalling into the
   current C16 kernel with per-process transfer state. Acceptance: files and
   directories, exact binary round trips, terminal input, pipes, partial I/O,
   errors and blocked operations. Exercise counts beyond the old C integer
   width and reject unsupported ranges without truncation. Unrelated C16
   processes continue to run while native I/O waits.
4. **Complete mixed process semantics.** Implement timer-driven native
   scheduling, eager-copy fork, wait/exit, signals and both directions of exec.
   Acceptance: a syscall-free native busy loop beside an interactive C16 shell,
   mixed pipelines, private parent/child globals/heap/stacks, shared file offsets,
   signal handling, failed exec rollback and repeated slot reuse. Require
   orderly shutdown and filesystem consistency. A trusted ring-0 fixture alone
   does not satisfy this gate.
5. **Provide the native SDK.** Supply target headers, libc, allocation, startup
   and syscall wrappers, with separate cache/install identities. Cross-compile
   a text utility and a small interpreter, then measure realistic workloads.
   Native SDK objects must not accidentally link against C16 libraries. Ordinary
   Clang output is the initial oracle; RCC migration is not a prerequisite.
6. **Extend the compilers and host RFL.** Add explicit layout and calling-ABI
   selection to the C frontends, shared IR/lowering and RFL. In parallel with
   that output-target work, build the Rust platform support needed to run a
   reduced RFL CLI in the guest, initially emitting existing compatibility IR.
   Follow the separate acceptance requirements below.
7. **Build the shared kernel as native C.** Audit the shared sources and build
   them against native target definitions, initially with Clang. Keep the
   compatibility application ABI through adapters; move services to native
   representations incrementally. Remove superseded conversions only after
   mixed-ABI regression checks pass. Hardware COW, SMP and further optimization
   follow their own acceptance gates.

The first implementation unit is the contract plus native execution slice,
not a bulk rebuild of existing applications. Each stage must retain a bootable
C16 path. Implementers should coordinate ownership of page tables, timer entry,
process state, loader and syscall integration before editing those shared areas.

## Compiler and shared source migration

Treat ISA, data layout, calling convention and OS services as separate target
properties. Today all shared targets return the Ripple layout, so changing the
ISA or one size table is insufficient. Audit C promotions, constant evaluation,
pointer arithmetic/casts, alignment, bitfields, initialization, aggregate calls
and varargs in both C frontends. The IR needs native pointer and byte-offset
semantics alongside bank-aware compatibility operations. The native backend
needs ordinary memory, stack and external-call lowering rather than mandatory
compatibility helper calls and function tokens.

Keep RFL's explicit integer widths: `i16` remains 16 bits. Make `usize`, C aliases,
references, strings, vectors, closures, ADTs, inline fields and alignment
target-aware. Its current cell widths are encoded in checking and lowering as
well as shared target metadata. Define C interoperability explicitly; internal
RFL representations need not become a public C ABI.

Share source-level algorithms and correct platform workarounds at their owning
layer. Make intentional C16 adaptations target-specific. Audit narrowed counters,
pointer casts, structure overlays and assumptions about `long`, `sizeof` and
octet counts. Keep file/network encodings explicit and independent of native
padding. Maintain target-specific SDKs and compiler output; do not duplicate
kernel or application trees solely to change their ABI.

## Hosting the Rust based RFL compiler

Running RFL inside native Riplix and making RFL emit native-layout programs are
independent. A compiler built as an LP64 executable can still emit C16 IR. The
first guest compiler should parse, check and emit that IR, then use existing
guest backend tools. Make optional backends, LSP and procedural-macro execution
separable features rather than prerequisites for the first CLI.

Use ordinary rustc x64 code generation and a pinned target/toolchain. A native C
ABI does not provide Rust `std`: supply startup, allocation, panic behavior and
the platform services used by the selected compiler/dependencies. Begin with
`panic=abort`; provide files, directories, environment, standard streams, clocks
and required synchronization/thread-local support. Threads, subprocesses, pipes
and file locking need actual implementations or explicit feature restrictions.
The current LSP uses a worker thread; the procedural-macro runner uses child
programs and threads for I/O. Enabling those features requires further gates.

Acceptance: cross-compile the CLI on the development host, install it on a
private Riplix image, compile real RFL modules to compatibility IR, build and
execute the result in the guest, and verify diagnostics and resource cleanup.
Then add native output, procedural macros and LSP with their own behavior tests.
Replace development-host paths and external-tool assumptions with installed
guest resources. The current Rust-to-RCC MIR prototype is a separate C16 subset;
it does not provide general `std` support for this compiler.

Cross-compiling RFL for the guest does not require rustc or Cargo in the guest.
An independently rebuildable RFL compiler additionally requires a guest Rust
toolchain; that larger self-hosting goal remains separate. See the
[self-hosting plan](../self-hosting/README.md),
[RFL dependencies](../../rfl/Cargo.toml), [Rust custom targets](https://doc.rust-lang.org/rustc/targets/custom.html)
and [bare-metal x64 support](https://doc.rust-lang.org/rustc/platform-support/x86_64-unknown-none.html).

## Compatibility and rollout

This is initially a freestanding x64 project. Hosted Fuse-X keeps its current
macOS integration and compatibility runtime until it has a separately validated
native-ABI implementation. Ring-3 entry and page-table switching on bare metal
are not a design for execution inside the macOS process.

Use separate target/cache/package identities and disposable test images.
Reject ABI mismatches at compile/link/load boundaries. Do not reinterpret old
RXE/FXE images, change read-v1 memory-view guarantees, or silently upgrade normal
disks as part of migration. Existing compatibility optimization and C16
self-hosting remain useful work and regression references.

Before implementation of each interface, settle its exact syscall numbers,
entry/register convention, image ABI marker, public structure layouts, TLS
mechanism and address map. Those details are not assigned by this direction
document. The existing [Ripple C ABI](abi.md), [memory model](memory.md) and
[C16 filesystem](c16-filesystem.md) remain authoritative for their current paths.
