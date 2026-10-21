# Freestanding native math

The offline app profile binds the existing `__rcc_host_math` IEEE-bit interface
using native x86-64 floating-point arithmetic and pinned Rust `libm` 0.2.16.
The C16 cell/pointer/call layout remains unchanged. Transcendentals are native
library routines; ordinary arithmetic already uses native compiler instructions.
There is no VM, softfloat service, host libc or OS call in this library.

Build with the standard SysV/SSE2 Rust code-generation ABI, no red zone and no
AVX. `boot.py` uses `x86_64-unknown-linux-gnu` solely for that ABI: the crate is
`no_std`, supplies its own fatal panic handler, and is statically linked into
the freestanding kernel. Install the Rust target with
`rustup target add x86_64-unknown-linux-gnu` if absent. Kernel symbol validation
rejects unresolved OS/libc dependencies. Garbage collection removes unreachable
prebuilt core/compiler-builtins sections before that validation.

Do not override the `x86_64-unknown-none` target's required soft-float ABI flags
to obtain hard-float calls: that combination is unsupported by Rust.
The baseline interrupt entry preserves all x87/SSE state; AVX needs a separate
XSAVE/context milestone. Existing cooperative native helper boundaries remain.

The scalar contract retains canonical NaNs, saturating integer conversions,
signed-zero min/max and explicit invalid-operation rejection. Native libm can
differ from other hosts in the last bit. Full floating environment/errno and
IEEE exception flags are not exposed by this service. Invalid requests and
panics stop explicitly; they never return an invented successful result.

Host validation:

```sh
cargo test --offline --lib --manifest-path tools/x86_64/math/Cargo.toml
```

Before binding the first application math client, the actual relocated kernel
runs a small native self-check covering sqrt, trigonometry, pow, hypot and NaN.
Failure stops boot/application loading. End-to-end acceptance additionally runs
awk's arithmetic and language-level results. The minimal app profile does not
require this helper or library.
