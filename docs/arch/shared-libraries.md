# Riplix shared libraries (RXE2)

Riplix/RVM applications normally use RXE2 shared modules. The kernel resolves
versioned libraries at `exec`, caches their instruction text, and maps the same
physical instruction banks into each process. Every process gets independent
library globals and initializers. Native Fuse-X uses its separate
[FXE shared-library implementation](../../fuse-x/EXECUTABLES.md); the two formats
share source APIs, not executable machine code.

## Building and installing

Ordinary Make, RFL, Rust-to-RCC, TLS, chat and browser builds use shared linking.
The disk builder installs libraries from `BUILD/lib` into `/lib/riplix` alongside
applications. The launcher preserves existing guest files during an update.
Stop the guest before installing a new kernel and its applications.

```sh
scripts/run-riplix.sh --build-only
scripts/run-riplix.sh --run-only
```

`RIPLIX_SHARED_LIBRARIES=0` selects legacy static RXE1 output. The linker mode is
part of incremental build stamps. RXE1 and RXE2 programs can coexist on the new
kernel; old kernels cannot load RXE2. The in-guest C assembler/linker still emits
self-contained RXE1 programs, which remain fully supported.

Host tools for custom libraries and C applications:

```sh
python3 ports/fuzix/tools/shared.py --cc library.c --soname libexample.rsl.1 \
  --build build/fuzix -o build/fuzix/lib/libexample.rsl.1
python3 ports/fuzix/tools/shared.py --cc application.c --build build/fuzix \
  --library "$PWD/build/fuzix/lib/libexample.rsl.1=/lib/riplix/libexample.rsl.1" \
  -o build/fuzix/application.rxe
```

The first command exports public C definitions; `--export NAME` restricts that
set. `--curses` adds the companion library. Existing `.pobj` files can also be
packed directly, with `--startup BUILD/user/entry.pobj` for applications.
Objects need current RCC metadata for public library exports.

Libraries currently include `libc.rsl.1`, `libcurses.rsl.1` and `libtls.rsl.1`.
Libc includes full floating formatting and Unicode support. TLS exports the
supported `ripple_tls_*`, `ripple_crypto_*` and `ripple_http_*` interfaces rather
than making optional wolfSSL internals public. Applications may continue to
contain their own smaller support libraries.

Copying an RXE2 executable to another disk also requires its dependencies at the
recorded guest paths. Copy libraries with their versioned names. Replacing a
library implementation without changing its public ABI preserves compatibility;
changing exported names or type hashes requires relinking consumers or a new
library version. There is no host-path fallback, `LD_LIBRARY_PATH`, runtime
`dlopen`/`dlsym`, or ELF symbol interposition.

## How execution works

The host packer rewrites RCC global-address instructions and uses the ordinary
Ripple linker for function placement, dead-code elimination, branch relaxation
and cross-bank code. No new CPU instructions or register calling convention are
needed. A module's internal calls and its imported calls are relocated once when
its text enters the code cache. Imported function pointers name the actual
function, so pointer comparisons and callbacks work across module boundaries.

The process's GP remains bank 8. It holds a private binding table:

- Cells 0–64: dependency initializer count and function pointers.
- Cells `128 + 2*slot`: module global base bank and GOT base offset.
- Cells 1024 onward: imported data pointers, as offset/bank pairs.

A module's globals start at a bank boundary in banks 9–30. Address calculations
load the module base through GP. Imported data projections use its GOT. Sparse
MMU pages mean alignment holes use virtual space without allocating physical
RAM. The stack stays in bank 31; the heap begins after the final module's globals.

Inline assembly that assumes globals live at fixed GP offsets needs adaptation;
ordinary banked symbolic global references are rewritten by the packer.

Code gets a stable virtual bank range derived from its physical code allocation.
Only modules in the process's dependency graph appear in its instruction map.
Code is immutable after publication. The cache holds dependency references, and
processes hold their own graph references. Fork retains shared code and copies
data mappings with the existing copy-on-write mechanism. Exec allocates fresh
library state, initializes dependencies in order, then initializes the application.
This gives each process one libc allocator, `errno`, stdio state and exit-handler
list, while preserving normal cross-library allocation and file-handle ownership.
Conflicting instances of one library name in a dependency graph are rejected.

Cached text survives process exit. If instruction space, metadata slots or RAM
are exhausted, unreferenced cache entries are evicted, including dependency pins
that become unused. Active and staged exec graphs stay pinned. The cache uses
the existing bounded instruction pool rather than reserving another memory pool.
Cache lookup checks package identity, header, dependency bindings and source
inode identity/ownership/timestamps. Libraries are reopened and their access and
mount permissions checked even on warm execs. Package identities are content
fingerprints supplied by the trusted build tools, not signatures.

Dependency paths, interfaces, imports, GOT bounds and relocations are validated
before replacing the process's data map. Missing or incompatible dependencies
leave the old process usable. As in the legacy loader, an I/O failure after the
new data map commits follows the kernel panic path. Concurrent in-place modification of executables
while loading is unsupported; install complete packages while the guest is stopped.

Limits are 32 modules per process, 8 direct dependencies per module, 1024 exports,
8192 imports and 16000 GOT entries per module, additionally bounded by the
available GP binding space, global virtual banks, physical RAM and instruction
pool. Public ABI hashes encode C16 layouts; pointer pointees are opaque. They do
not replace complete C header/prototype compatibility checking.

## On-disk envelope

All integers are little-endian host octets. The header is 128 octets:

| Offset | Field |
|---|---|
| 0 | `RXE2` |
| 4 | u16 bank size, 64000 |
| 6 | u16 flags: 0 application, 1 library |
| 8 | u32 entry instruction index; library `0xffffffff` means no initializer |
| 12 | u32 instruction count |
| 16 | u32 logical global-data cells |
| 20 | u32 relocation count |
| 24 | u32 metadata octets, padded to a multiple of 256 |
| 28 | four u16 counts: dependencies, exports, GOT entries, imports |
| 36 | 32-octet public interface SHA-256 |
| 68 | 32-octet package SHA-256, computed with this field zeroed |
| 100 | u32 initialized data-prefix octets; remaining cells start zero |
| 104 | NUL-terminated module name in 24 octets |

Metadata follows the header: dependency records (160 octets), exports (104),
imports (40), GOT entries (12), then alignment padding. Payload contains normal
8-octet Ripple instructions, initialized global octets and 16-octet relocation
records. Field definitions are in
[`format.py`](../../ports/fuzix/tools/shared/format.py); kernel parsing and
validation are in
[`shared-loader.inc`](../../ports/fuzix/ripple/shared-loader.inc).

## Verification and measurement

```sh
python3 ports/fuzix/tools/test_shared.py --benchmark --tls
python3 ports/fuzix/tools/test_curses.py --build build/riplix-shared
python3 ports/fuzix/tools/measure_shared.py
```

The RXE2 regression uses disposable disks. It covers shared libc ownership,
callbacks, function pointer identity, signals, setjmp/longjmp, globals across a bank boundary, fork/COW,
independent simultaneous execs, atexit, rejected metadata and failed-exec rollback,
a shared RFL application, and bounded cache eviction/reload. The optional TLS
fixture verifies TLS 1.2/1.3 downloads and rejects an invalid certificate.

The startup benchmark links the same `cat.pobj` statically and dynamically, then
interleaves nine launches of each under RVM JIT. It records raw measurements in
`BUILD/startup-benchmark.json`. A sample run reduced `cat` from 184092 to 13792
host octets (92.5%). Median warm command-to-prompt time fell from 328 to 220 ms
(33%); a later run during concurrent compilation measured 1212 to 769 ms (37%).
These include shell/fork overhead and are sensitive to host workload.
The common library was already resident through the shared shell. These are
not claims of a 33% speedup for every application or cold OS boot. First use
still loads a complete library, which can cost more than a small static subset.

The six-program size sample below links the exact same application objects both
ways. Counts are host octets; C16 disk storage uses wider character cells.

| Program | Static RXE1 | Shared RXE2 |
|---|---:|---:|
| cat | 184092 | 13792 |
| sh | 1838233 | 1140040 |
| mux | 2686370 | 1872672 |
| cursescheck | 1763859 | 364600 |
| htget | 17311349 | 5120 |
| chat | 19735420 | 2203568 |

Including libc (2630784), curses (843144) and TLS (17789120) once, the sample
falls from 43519323 to 26862840 octets: 38.27% less. Raw sizes are recorded in
`BUILD/shared-sizes.json`. The full application-manifest check built 154 RXE2
programs/aliases and verified that all their direct dependency interfaces match
the installed packages. Real guest tests also passed 115 curses assertions,
the graphical-terminal/raster suite, a shared Rust utility alongside a static C
utility, and a shared program built by the portable C frontend/backend.
Separate large-image smoke tests loaded Lynx, NetSurf and chat successfully and
checked version/help output and offline chat listing.
