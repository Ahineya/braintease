# Native RCC/RFL target — first milestone

`--target native` compiles shared RCC/RFL IR through Cranelift into an object
and links a standalone host executable. Execution has no Ripple instruction
fetch/decode loop. The default target remains `ripple` and its ISA is unchanged.

From the repository root:

```sh
cargo build --release -p rfl -p rcc-driver -p rcc-native-runtime

target/release/rfl run rfl/examples/beep.rfl --target native

target/release/rfl compile rfl/examples/beep.rfl --target native -o build/beep
build/beep

target/release/rcc compile c-test/tests/core/float/native_ir_bridge.c \
  -I runtime/include --target native -o build/float-c
build/float-c

target/release/rfl compile rfl/examples/beep.rfl --target native --emit-ir
```

`rfl run` compiles and links a temporary executable, runs it, and removes it.
For execution-only measurements, compile once and run the resulting executable.
`--jit` still selects the Ripple VM JIT and cannot be combined with the native
target. Flags can appear after the source filename.

## Target selection and floating point

RFL evaluates item attributes before name checking and import loading:

```rfl
#[cfg(target_has = "host_math")]
extern "C" {
    fn __rfl_host_sin(x: f64) -> f64;
}

#[cfg(target_has = "host_math")]
export fn sin(x: f64) -> f64 { __rfl_host_sin(x) }

#[cfg(not(target_has = "host_math"))]
export fn sin(x: f64) -> f64 { portable_sin(x) }
```

The stdlib's `sin` and `cos` use this selection. The language server currently
checks the Ripple target. `floor` and `sqrt` also have
native runtime bindings. This needs item configuration, not a macro system.
Supported predicates are `target_has`, `target`, `target_arch`, `all`, `any`, and
`not`. Capabilities are `host_math` and `native_float`. `target_arch` is
`ripple16` for Ripple and the selected host architecture for native. Unknown
capabilities and malformed conditions are errors. Inactive items still have to
parse, but their names and imports need not resolve. Attributes compose with `repr(C)`.

Frontends now emit typed `float.f32.*` / `float.f64.*` intrinsics. Explicit
`float.value.*` and `float.bits.*` reinterpret the legacy storage representation;
these are distinct from numerical conversions. Ripple selects software helpers
in backend legalization. Cranelift selects hardware arithmetic, comparisons,
and conversions. Float-to-integer conversions truncate and saturate, with NaN
becoming zero, matching the existing software helpers. No fast-math mode is
introduced. Host trigonometry need not return identical last bits to the
portable algorithms.

IR records its target in JSON and text. Old IR defaults to Ripple. The native
backend requires native-targeted IR, and the Ripple backend rejects native IR;
recompile source to change a target-dependent stdlib selection.

## Compatibility layout and ABI

This milestone deliberately retains the existing data model: 16-bit cells,
16-bit C `int`/`char`, 32-bit logical bank/offset pointers, and 64,000 cells per
bank. `rcc-ir::target::DataLayout` owns checked IR storage sizing. Logical memory
is a runtime-owned allocation; generated native arithmetic and control flow do
not execute VM instructions. Loads, stores, pointer arithmetic, allocation,
and MMIO currently go through checked runtime adapters.

RCC-generated C and RFL share a **private native ABI**: a hidden runtime context,
one `u64` carrier per scalar parameter, and one result carrier. Integer widths
are preserved; floating carriers contain IEEE bits at memory and call
boundaries. RFL retains its activation-record driver and closure protocol.
Function pointers are checked logical tokens mapped to actual code addresses.
This supports C callbacks into RFL without exposing a host pointer as a logical
pointer.

This is not the platform C ABI. Do not link ordinary system C objects against
these symbols. Runtime bindings are explicit and prefixed, so e.g. logical
`malloc`/`puts` cannot accidentally resolve to host libc. Calls to unknown
externals fail explicitly. Defined C functions can be imported into RFL as before.
Variadic C functions use a hidden logical pointer to arguments packed in the
caller's frame, preserving promoted scalar widths and supporting `va_copy`.
`main` accepts zero arguments or `(int argc, char **argv)`; process arguments
are copied into logical memory.

Host data layouts, direct native RFL calls, native pointers, aggregate ABI
classification, host C interoperability, and cross-compilation are later work.
The compatibility global bank and logical stack/heap limits still apply.
Dynamic `alloca` and Ripple inline assembly are rejected.

## Runtime and devices

`rcc-native-runtime` owns logical memory, the allocator, output, function tokens,
and device adapters. It reuses `rvm`'s device protocol and RGB565 presentation
implementation, without constructing a VM. Supported services include
`putchar`, `puts`, `getchar`, allocation, cell-oriented memory/string functions,
MMIO, exit, integer absolute values, and the selected host-math bindings.
C file streams use runtime-owned handles and checked logical buffers: host
files transfer octets into compatibility cells. Opening, reading, writing,
seeking, EOF/error checks, closing, removing and renaming files are supported.
Host `FILE *` values never cross the private ABI.

```sh
build/graphics
RCC_NATIVE_HEADLESS=1 build/graphics
RCC_NATIVE_FRAME=build/frame.rgb565 RCC_NATIVE_FRAME_NUMBER=10 RCC_NATIVE_TIMEOUT=30 build/graphics
RCC_NATIVE_INPUT_SCRIPT=path/to/replay.json build/graphics
build/network/monitor/native/monitor
RCC_NATIVE_NET=off build/network/native/ping
RCC_NATIVE_NET=/path/to/nic.sock build/network/native/ping
```

The frame option saves little-endian RGB565 pixels at the requested completed
presentation (default 1) and exits. `RCC_NATIVE_TIMEOUT` bounds execution in
seconds and reports failure if the limit is reached. Requesting a frame from a program which never presents
one is an error. Legacy RGB565 front/back buffers and the descriptor-based
extended display use the existing device implementation. Loop polling allows
window close to stop even a loop with no memory accesses. Enabling RGB565 or
RGB565_EXT opens a window automatically; console programs do not initialize
the window system. The main thread waits for a display request while native
code executes on a worker. `RCC_NATIVE_HEADLESS=1` suppresses the window, and
frame captures are headless automatically. Replay format is the same as RVM's input scripts. Tick requests
complete immediately headlessly and are paced at the shared tick rate with a
window. `RCC_NATIVE_UNTHROTTLED=1` disables visual pacing.
Live keyboard events are collected by input commands independently of tick
grants; applications can poll input without advancing their simulation clock.
Tick-scheduled replay retains deterministic delivery.
Networking uses the same mailbox through `<vm_network.h>`, with one octet per
compatibility cell and bounded host queues. On macOS the first `vm_net_info`
query automatically starts host-only Ethernet using an embedded native helper;
only that helper needs `sudo` authorization. Console programs that do not query
the NIC never initialize networking. `RCC_NATIVE_NET=off` disables attachment,
`shared` explicitly selects Internet sharing, and a socket path reuses an
external Ethernet helper. `RIPPLE_NET` is a shared fallback setting. See the
[networking contract and ping examples](../docs/networking/README.md).

TEXT40 rendering, storage devices, and VM interrupts are not implemented in this
milestone and report runtime errors when requested. TTY input is available via
`getchar`; the asynchronous MMIO TTY input queue is not connected. Native
executables use the host process exit status (low eight bits on Unix).

Linking currently supports macOS and Linux, with execution validated on macOS
ARM64. A host C linker is required. The runtime is found at
`target/release/librcc_native_runtime.a`; `RCC_NATIVE_RUNTIME` overrides its path
(e.g. with `CARGO_TARGET_DIR`). `RCC_NATIVE_CC` overrides the linker executable.
On macOS the SDK comes from the selected developer toolchain through `xcrun`.

## Multi-file C programs and Doom

Compile each translation unit with `rcc compile file.c --target native --emit-ir`
and save stdout to a separate `.ir` file. Link them together with:

```sh
target/release/rcc link-native first.ir second.ir -o build/program
ports/doom/run.sh --target native --scene e1m1
make -C ports/doom test-native
```

The linker isolates static symbols per unit, resolves shared definitions and
removes unreachable functions/globals. Incomplete unused extern declarations
need no runtime binding. Doom uses its portable C kernels, the port's C formatter
and scanf implementation, logical file adapters and the shared host devices.
`TARGET=rcc-native` builds this executable; Doom's older `TARGET=native` remains
the Clang/GCC reference for validation.

## Validation

An opt-in cooperative process prototype is available through
`<native_process.h>`. It saves live continuation values, uses copy-on-write
private pages, and provides an explicit shared kernel arena. Programs without
these service calls keep flat RAM and ordinary native calls. See
[native processes](PROCESSES.md) for standalone examples, the API, limits and
tests; these do not require the FUZIX port.

Build the release runtime first, then:

```sh
cargo test -p rcc-ir -p rcc-native -p rcc-native-runtime
cargo test -p rcc-driver --test native
cargo test -p rfl --test target_cfg --test native
cargo test -p rfl
./rct native_ir_bridge test_float_basic test_double_basic test_double_precision \
  test_static_float_to_integer test_struct_double_indirection test_softfloat_bits \
  test_printf_float test_sscanf_float test_va_arg_double --timeout 15
```

Native tests link and execute real objects. They cover integer boundaries,
loops/phis, IEEE special values, conversion rules, managed strings/arrays/slices,
copy-on-write vectors, allocations spanning banks, closures, 100,000 tail calls,
C callbacks and floats, target-selected imports/math, fault paths, and exact
RGB565 pixels. Runtime tests check allocator reuse, bank crossings, wide loads,
logical stack restoration, and framebuffer/RAM ownership.

## Initial measurement

On the development ARM64 Mac, unmodified `rfl/examples/beep.rfl` emitted 44,100
samples in a median **56.0 ms** as a native executable, versus **4.24 s** in the
RVM JIT (three process runs each, stdout discarded). Source compilation and
linking were excluded; the VM measurement includes JIT startup. The same
64,000-cell data ABI was used, with 16,777,216 cells for the headless VM. This
is a workload-specific result, combining hardware arithmetic and host sine;
it is not an estimate for integer or graphics workloads. Native sample output
agreed with host sine within 5e-7 after decimal formatting.
