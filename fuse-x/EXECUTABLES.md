# Fuse-X native executables

Fuse-X applications can be compiled independently, installed in the guest
filesystem, and executed without rebuilding the hosted kernel. The initial SDK
reuses the existing Riplix libc sources and retains RCC's compatibility data
layout. Packages contain native code for the build host's CPU and operating
system. macOS and Linux packages are distinct.

## Compile and install

Build the kernel and SDK utilities first:

```sh
scripts/run-fuse-x.sh --build-only
python3 fuse-x/cc.py fuse-x/examples/hello.c -o /tmp/hello.fxe
node fuse-x/image.mjs put fuse-x/build/ROOTFS.IMG /bin/outside /tmp/hello.fxe
scripts/run-fuse-x.sh --run-only
```

Run `outside some arguments` in the guest shell. The compiler accepts multiple
C source files, `-I` include directories, and `-D` definitions. `main` may take
zero arguments, argc/argv, or argc/argv/envp. The startup wrapper initializes
libc, calls main, flushes stdio, and exits through Riplix.

Install files while Fuse-X is stopped. `image.mjs put` saves the old image as
`.bak` or `.bak.N` and atomically writes the updated filesystem. Use a separate
image for experiments. Alternatively, compile into a Mac folder and launch with
`scripts/run-fuse-x.sh --run-only --share /path/to/folder`; the resulting package
can run directly as `/mnt/host/program.fxe`, retaining its host executable bits.

Normal builds inventory the ported application set and install successfully
built local applications, including the existing Bourne shell as `/bin/sh`.
Bootstrap `/init`, `/bin/fxsh`, `hello`, and `check` retain prelinked entries.
Network applications install normally and use the shared hosted socket adapter.
See [application coverage](APPLICATIONS.md).

The SDK caches native IR from portable libc and its hosted syscall bridge.
File operations, stdio, directory access, environment, formatting, allocation,
and process operations enter the same Riplix kernel as the bootstrap programs.
New SDK allocators opt into banks 48–239 on first growth: 12,288,000 C16 cells
(24,000 KiB) of heap per process, disjoint from frames, argument storage,
application globals and the shared kernel arena. Large individual requests can
span this region, including full-HD RGB565 canvases. Growth remains bounded and
allocations can fail; this is address-space capacity, not reserved resident RAM.
Old packages retain their original banks 8–30 and allocator limits. The arena
switch does not clear or merge storage in the intervening argument/global
banks, and needs no package or C ABI change.
Custom signal handlers
run after completed system calls, following Riplix's
one-shot signal semantics. Continuation-aware `setjmp`/`longjmp` preserve live
execution state across nested calls and fork. A handler may call the kernel or
leave through `longjmp`. Signals are delivered after completed syscalls and at compiler-generated
user-code checkpoints, including inside computation loops. Rebuild old packages
to enable loop checkpoints. Kernel callbacks and their signal handlers remain
uninterrupted until they block or return. Socket I/O and DNS
use the guest protocol stack and existing Ethernet device. Build and runtime coverage are recorded separately
in the application inventory; untested options and modes remain unvalidated.

Aggregate return values temporarily point into the released callee frame.
The caller copies them before its next ordinary call; automatic timer callbacks
are deferred across that return boundary so they cannot overwrite the result.

## Package and runtime interface

### Guest shared libraries

Native application builds use ABI 3 shared modules by default. Use `cc.py --static`
for a self-contained package, or set `FUSE_X_SHARED_LIBRARIES=0` for an entire
static build. `--shared` explicitly selects the default. Link-mode changes
invalidate application caches, including graphics and guest compiler tools.
Old kernels cannot load ABI 3; restart with the updated kernel before using
shared packages.

```sh
python3 fuse-x/cc.py fuse-x/examples/hello.c --shared \
  --build fuse-x/build/shared-test -o fuse-x/build/shared-test/hello.fxe
```

The SDK builds versioned `/lib/fuse-x/libc.fxl.1` and companion curses/INI/theme
libraries. Native chat, htget and Lynx builds also
use `/lib/fuse-x/libtls.fxl.1`. TLS exports the supported `ripple_tls_*`,
`ripple_crypto_*`, and `ripple_http_*` interfaces; optional wolfSSL implementation
APIs are not part of that shared-library contract. Source/IR SDK caches and
shared library outputs are fingerprinted and reused across application builds.
`image.mjs create/install --apps BUILD/apps` installs the versioned packages
from `BUILD/lib` alongside applications. Stop the guest before updating its disk.
Copying a shared executable alone to another guest requires installing its
library dependencies there as well.

Custom libraries can be built directly:

```sh
target/release/rcc link-shared library.ir --soname libexample.fxl.1 \
  --export example_function --export example_global -o libexample.fxl.1
python3 fuse-x/cc.py app.c --shared \
  --library "$PWD/libexample.fxl.1=/lib/fuse-x/libexample.fxl.1" -o app.fxe
```

Omitting `--export` exports public definitions. Internal functions and data
remain module-local. The linker checks declared signatures and call arities,
including variadic packing. Dependency records contain SONAME, absolute guest
path and a SHA-256 interface identity. Version the SONAME for incompatible
changes, including layouts of structures accessed through pointers: pointer
pointees are opaque in the scalar calling interface. No host library search
path or host libc symbol resolution participates in guest linking.

ABI 3 adds module exports/imports and pointer relocations. A dense little-endian
initializer prefix sits between the JSON manifest and native payload; omitted
trailing cells are zero. The payload length continues to count native code
only. Initialized data/function pointers, imported globals and pointer addends
are relocated at load time. ABI 1/2 envelopes retain their original layout.

Native code is cached by package content and held while processes/staged loads
reference it. A compatible live SONAME/interface can be reused without another
payload read or native load, after the kernel opens and checks the requested
guest file's permissions and mount policy. A live library therefore keeps its
current code until its final owner exits; replacing the file does not hot-patch
running programs. Host helper bindings contain no guest data addresses.

Each executable load builds its own binding graph and private initial data.
Application and dependencies use one libc instance per process, sharing its
allocator, errno, environment, stdio and exit handlers. Fork retains code and
shares data copy-on-write; exec resets every module's globals. Function pointers
and callbacks use the continuation protocol across module boundaries. Failed
dependency resolution does not replace the calling program's image.

Current limits are 32 modules, at least one function definition per module,
and the existing aggregate 16-bank global region
per program. There is no guest `dlopen`/`dlsym`, ELF symbol interposition or
automatic constructor/destructor protocol; startup performs libc initialization
and normal `exit` runs registered handlers. Dependencies are resolved at exec.
Riplix/RVM builds now use their own [RXE2 shared modules](../docs/arch/shared-libraries.md),
with module-aware instruction mappings and private guest binding tables. Legacy
static RXE1 programs remain supported. Native FXE libraries cannot be entered
by the Ripple CPU.

Validation and reproducible size reporting:

```sh
python3 fuse-x/test_shared_libraries.py --build fuse-x/build/shared-test
python3 fuse-x/test_shared_tls.py --build fuse-x/build/shared-test
python3 fuse-x/measure_libraries.py --capture-baseline fuse-x/build
python3 fuse-x/measure_libraries.py --build fuse-x/build/shared-test
```

The size tool separates application files from the complete dependency closure,
counting each shared library once. Its baseline JSON records host octet sizes;
physical C16 disk storage uses two host octets per stored package octet.
Capture the baseline before converting those application outputs.

Measured macOS ARM64 build, 2026-10-19 (decimal host MB):

| Application | Static FXE | Shared FXE | Application reduction |
| --- | ---: | ---: | ---: |
| chat | 9.103 | 2.206 | 75.8% |
| htget | 6.589 | 0.020 | 99.7% |
| Lynx | 34.723 | 25.971 | 25.2% |
| gterm | 1.719 | 1.008 | 41.3% |
| mux | 1.566 | 1.299 | 17.0% |

Common libraries add 1.093 MB for libc, 0.352 MB for curses, and 5.691 MB for TLS,
once per installation. The complete selected set drops from 53.700 to 37.641 MB
(29.9%). Chat's JSON manifest drops from 1.320 MB to 25.5 KB, plus a 26.7 KB
binary initialization prefix. These are package sizes, not measured resident
memory or startup speed. The baseline captures the existing applications at
the start of this change; exact future builds can differ with source/configuration.

### Static packages

`rcc link-app startup.ir application.ir libc.ir ... -o program.fxe` emits the
package after linking application IR. Its startup `main` accepts one logical
pointer to `{ unsigned argc; char **argv; char **envp; }`; `cc.py` generates this
wrapper around an ordinary C main.

The package envelope contains:

| Offset in octets | Contents |
| --- | --- |
| 0 | Eight-byte magic `FUSEX001` |
| 8 | Manifest length, little-endian u32 |
| 12 | Native payload length, little-endian u32 |
| 16 | UTF-8 JSON manifest, followed by the native shared-object payload |

The manifest records ABI version 2, CPU, host OS, entry token, function exports
and arities, required runtime helpers, global storage size, errno address, and
global initializers including function-pointer relocations. The current package
limit is 64 MiB. The kernel streams packages through a 4,096-cell buffer
to process-owned host storage, rather than reserving the entire package in the
shared guest arena. Small applications retain globals in bank 1. Larger images
automatically use banks 32–47, up to 1,024,000 cells including the reserved first
cell. The manifest's `global_bank` selects the region; the loader validates all
initializer spans and reserves it from the allocator. Globals are private after
fork and reset on exec, including values that cross bank boundaries. Older ABI 1
packages without `global_bank` still load with their original single-bank layout.
All
package octets occupy individual compatibility cells in the C16 filesystem.

Host code relocation uses the platform dynamic loader. The runtime stages the
payload in a private temporary directory, loads it locally, resolves its declared
function exports, and binds an explicit helper table. Applications do not link
another copy of the native runtime or resolve arbitrary kernel symbols. The
runtime checks each helper name and arity against its supported interface.

The kernel streams packages in 4,096-cell chunks, releasing filesystem buffers
before servicing clocks/devices and scheduling other processes (about every
5 ms). `rcc_hosted_image_prepare` transfers the owned package to a two-worker
native loader pool. Those workers never borrow Runtime or guest memory. The
kernel polls with `rcc_hosted_image_poll`, yielding between polls; each poll
installs at most 128 function bindings. A pending result is zero, failure is
`0xFFFFFFFF`, and success is the staged entry token. The older synchronous
load/finish helpers remain available for compatibility.

Each process owns its construction, pending job and staged result. Cancellation
never joins a worker; abandoned queued jobs are skipped and completed orphaned
results release their library and temporary directory. A partially installed
function table cannot be entered as an application. Global function-token ranges
are allocated atomically across workers. Validation errors leave the old program
intact. Signal delivery is deferred until the loader releases its resources and
exec commits or returns.

Each load receives distinct logical function tokens. Code constants and global
function-pointer initializers resolve into that image's token namespace. Every
process retains its loaded code across fork and while entering kernel syscalls.
Exec restores the selected image's initial globals and replaces its saved frames.
Once no process or staged exec owns an image, its native library, function-table
entries, and temporary files are released.

Validation and loading precede the exec commit. Invalid headers, ABI/CPU/OS
mismatches, bad lengths, unsupported helpers, invalid data relocations and missing
entries return ENOEXEC. Failed loading preserves the caller's data and frames;
argument setup failure discards a staged image before returning.

Packages contain trusted native machine code and share the host process. They
are not an isolation boundary. Scheduling remains
assisted by compiler checkpoints, memory accounting provisional, and runtime
faults still stop the host session. Checkpoints use an additive runtime helper;
older packages can still load but have no loop scheduling checkpoints.

## Validation

```sh
make -C fuse-x test
```

The executable checks compile a new program after the kernel already exists,
install it, and verify that the kernel binary is unchanged. They exercise real
cat/ls/cp, persistence, bank-spanning allocation, initialized function pointers,
private memory after fork, shared descriptor offsets, exec environment and global
reset, code retained by children across parent exec, and malformed-image rollback.
Repeated launches also check that retired images are unloaded while the kernel
continues running and that temporary code files disappear after shutdown.

`python3 fuse-x/test_scheduling.py` exercises two Lynx launches, concurrent exec,
a CPU-bound loop and 2,000 terminal redraws through mux during 20 seconds of PCM
playback. It checks a 10 ms sleeper's worst wakeup gap (200 ms ceiling), successful
exec, zero logical missing PCM frames and zero discarded clock catch-up. All disks
are disposable. `--live-audio` feeds a silent WAV through the actual host speaker
callback and additionally checks recovered physical starvation gaps (100 ms
ceiling); it fails if no speaker opens. Thresholds are configurable for slow hosts.
Physical counters are independent of logical PCM underruns and remain zero in
muted runs. They exclude startup, deliberate pause/stop and terminal drain silence.
