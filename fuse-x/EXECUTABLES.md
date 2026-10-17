# Fuse-X native executables

Fuse-X applications can be compiled independently, installed in the guest
filesystem, and executed without rebuilding the hosted kernel. The initial SDK
reuses the existing FUZIX libc sources and retains RCC's compatibility data
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
libc, calls main, flushes stdio, and exits through FUZIX.

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
and process operations enter the same FUZIX kernel as the bootstrap programs.
The allocator uses the hosted bank-aware brk/sbrk region. Custom signal handlers
run after completed system calls, following FUZIX's
one-shot signal semantics. Continuation-aware `setjmp`/`longjmp` preserve live
execution state across nested calls and fork. A handler may call the kernel or
leave through `longjmp`. Signals are delivered after completed syscalls and at compiler-generated
user-code checkpoints, including inside computation loops. Rebuild old packages
to enable loop checkpoints. Kernel callbacks and their signal handlers remain
uninterrupted until they block or return. Socket I/O and DNS
use the guest protocol stack and existing Ethernet device. Build and runtime coverage are recorded separately
in the application inventory; untested options and modes remain unvalidated.

## Package and runtime interface

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
