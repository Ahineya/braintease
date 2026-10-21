# Guest x64 OS bootstrap

The first bootable milestone passes on Ubuntu/KVM (2026-10-20). A guest-built
portable C frontend compiles all 71 C16 translation units in the current minimal
x64 kernel/embedded-program inventory inside Riplix. The extracted IR matches
the host reference byte for byte. A kernel emitted from that IR boots and passes
shell, process, pipe, timer, independent-application and two-boot persistence
checks. This is a **guest frontend OS build with a host backend**, not a fully
self-hosted kernel or an LP64 migration.

## Portable kernel and image mode

The next bootstrap implementation removes Cranelift, LLD and host image
packaging from the C16 kernel path. The portable tools now provide:

```sh
rcc-qbe --kernel start.ir --kernel process.ir app.ir -o kernel.ssa
qbe -t amd64_sysv kernel.ssa -o kernel.s
rcc-x64-as kernel.s -o kernel.o
rcc-x64-ld --kernel-elf kernel.o PLATFORM_OBJECTS... -o kernel.elf
rcc-x64-ld --efi-loader loader.obj --kernel-payload kernel.elf -o BOOTX64.EFI
rcc-x64-ld --boot-media BOOTX64.EFI -o boot.img
```

`--kernel` scopes internal names per translation unit, validates cross-unit
function/global declarations, retains symbols reachable from `main` and assigns
kernel globals to shared bank 240. Application globals retain private bank 1 or
32..47. Startup reserves shared storage, writes symbolic/numeric initializers,
registers continuation tokens, binds errno and starts the existing hosted
scheduler. Kernel execution has no application loop checkpoints. Native service
calls use the explicit private carrier ABI; process operations still suspend
through runtime requests. Unknown imports, unresolved bindings, incompatible
ABIs and capacity violations fail before publishing output. External service
addresses used as callback tokens are currently rejected.

ELF output is a static PIE with separate executable, read-only and writable
segments. BSS occupies virtual memory rather than a huge output buffer.
64-bit addresses become checked `R_X86_64_RELATIVE` records; GOT references use
real slots, including memory operations that cannot safely become `lea`.
The EFI mode links one self-contained frozen x64 COFF loader with the new ELF
payload, emits PE32+ and base relocations, and needs no COFF payload assembler.
It is not a general Windows linker or COMDAT/library implementation.
Boot media is a streamed 16 MiB FAT16 superfloppy with `EFI/BOOT/BOOTX64.EFI`.
All modes publish by temporary-file rename; failures preserve existing outputs.

The ROM/minimal/virtio platform capsule is still a host input. Its legacy
`root.o` embeds a root image that the separate virtio-root profile never uses;
the portable link explicitly omits that object. The capsule still validates
its hash, and the report names the unused input. Host LLD previously discarded
it through section garbage collection. General ELF section garbage collection
and arbitrarily large unused input objects are separate work.

`test_os_kernel.py` checks the accepted guest frontend outputs, lowers and
assembles the kernel, builds ELF/EFI/media, and runs the existing two-boot OS
suite. Host execution of these portable tools passes with the frozen platform:
processes, shell/scripts, pipes, timer preemption, independent applications and
persistent writes all work. `--guest-tools DIR --base BASE` runs the backend,
assembler, linker and media construction inside Riplix. It compares combined
QBE with the host lowerer, compares ELF/EFI/media on the actual guest-produced
object, checks the stopped filesystem, then boots that image twice. QBE register
allocation may differ between guest heapsort and host libc qsort; executable
acceptance and same-object link parity cover that boundary.

Guest source rebuilds use separate frozen snapshots. The first updated lowerer
(18 C units, before the scratch-lifetime fix below) passed its guest source
build, unchanged rebuild, same-object link parity, execution and fresh boot;
the Make step took 984.288 seconds under KVM. The linker rebuild exposed QBE's
memory cost for one large EFI-emission function. Splitting it into named phases
reduced the host portable-pipeline peak from 261,520 to 8,344 KiB, and that EFI
unit subsequently compiled successfully inside the guest.

The first combined guest kernel lowering reached the bounded application heap
limit. Function liveness/dominance scratch, CFG validation matrices and generated
operand text were incorrectly retained for the whole module. They now have
explicit function lifetimes; redundant source-module tables are reclaimed after
linkage. `native/test_memory.py` failed with an 8 MiB compiler-allocation budget
before the fix and passes at 5,102,356 bytes afterward. The registered C/rct
fixture is the passing semantic control. The complete 71-unit kernel QBE remains
byte-identical (`96f48bba...d7348bd3502`). These local checks do not establish a
successful guest run of the fixed lowerer.

Long build commands run through guest scripts, bypassing the interactive
terminal's canonical-line limit. This changes no terminal or kernel limits.
`--lowering-only` accepts a combined lowering checkpoint; `--reuse-lowering DIR`
verifies its tool, source, base-image and output hashes before continuing.

Ubuntu was released for the user's hardware testing on 2026-10-20. Its final
private guest synced and shut down cleanly; no build QEMU processes remain.
Resume from the immutable source/result directories, keeping these distinct:

- `build/ubuntu-kernel-tools-results-v1`: accepted first kernel-mode lowerer.
- `build/ubuntu-kernel-tools-source-v3`: phased ELF/EFI/media linker sources.
- `build/ubuntu-kernel-linker-run-v3/source-rcc-x64-ld-root.img`: clean interrupted
  linker source build; completed objects remain there, not yet accepted as a tool.
- `build/ubuntu-kernel-guest-lowering-v2`: retained bounded-heap failure evidence.
- `build/ubuntu-kernel-qbe-v6`: passing portable host-tool image/two-boot evidence.

Rebuild the scratch-lifetime changes in the guest, finish/revalidate the linker,
then execute the complete in-guest image pipeline and its boot suite. The final
guest image milestone remains pending. The original 71-unit guest frontend and
per-unit QBE assessments remain distinct evidence.

```sh
python3 rcc-portable/native/test_kernel.py
python3 rcc-portable/x64-link/test_kernel.py
python3 rcc-portable/x64-link/test_efi.py
RCC_TEST_ACCEL=kvm python3 rcc-portable/native-sdk/test_os_kernel.py \
  --source build/guest-os-source-v6 --os-build build/ubuntu-os-checked \
  --host-platform build/minimal-platform-capsule --output build/guest-kernel-image \
  --guest-tools PATH/TO/FOUR-FXE-TOOLS --base PATH/TO/COMPILER-TEST/normal \
  --timeout 5400
```

The platform C/assembly, EFI loader, existing root userspace packages and
source-tree generation remain recorded host bootstrap inputs. Consequently
`fully_guest_built` remains false even when `guest_image_built` passes. This
milestone makes the C16 OS compilation and final image tools independent; it
does not migrate the platform compiler to LP64 or rebuild every root package.

## Original guest frontend boundary

```text
host: freeze source adapters, headers and exact build options
guest Riplix: raw C + preprocessing -> RCC IR (guest-built rcc-c, guest Make)
host: RCC IR -> native kernel object (existing Rust/Cranelift backend)
host: combine it with native platform objects -> kernel ELF -> EFI image
QEMU/KVM: boot that kernel, execute checks, shut down, boot again
```

The fixed native object set covers startup/interrupt assembly, hardware/runtime
C, console/input, ACPI/timers, PCI/block I/O and embedded root storage. The EFI
loader is also host compiled. Independent applications in that root remain
explicit host-built packages. The optional Rust math object belongs in the same
host-input inventory when an offline application profile needs it; the initial
ROM/minimal/virtio profile does not require it.

Keep those objects as a versioned bootstrap capsule: target, flags, source and
object hashes, exported runtime ABI and expected dependencies. Reusing them is
a useful intermediate step. Do not count them as guest-built code or silently
refresh them during a long guest compilation. `test_os_guest.py` records the
backend binary, hardware objects, loader, linker script and root-seed hashes.

`platform_capsule.py` makes that boundary concrete. It copies only the fixed
platform objects, loader, root seed, linker script and profile record into a new
directory, records every hash and rejects changed/missing objects on use. The
host-generated C16 kernel object is deliberately excluded. A capsule can replace
the live build directory in `--host-platform`:

```sh
python3 rcc-portable/native-sdk/platform_capsule.py \
  --base build/x86_64-boot/test --output build/minimal-platform-capsule
```

Logical C16 addresses and ordinary hardware pointers remain different. The
existing native runtime service boundary translates them; linking two x64
objects does not establish compatible data layouts or calling conventions.

## Source preparation and acceptance

`source_os.py` derives the exact source/flag inventory from the current
freestanding Make rules and linked IR lists. It captures generated adapters and
relative includes, writes a guest Makefile, and fingerprints every input. It
refuses an existing output directory so a source snapshot is not refreshed by
accident.

Strict-C corrections apply only to that copied source tree and are listed in
its manifest, with original hashes and each replacement:

- Make runtime string declarations agree with the existing const-correct kernel
  prototypes.
- Declare `namecpy` before the namespace-cache include uses it.
- Give address-validation calls explicit pointer conversions and match device
  output variables to their declared `unsigned short` type.
- Namespace the unused legacy `net_poll(void)` declaration consistently with the
  socket bridge's existing local alias; it is unrelated to the new network stack.

These adapters preserve C16 storage widths and runtime behavior. No shared
kernel, network or hardware-owner sources are changed by the bootstrap harness.
They are temporary source-conformance work to reconcile at the owning layer.

Use a Clang-built reference frontend for exact IR parity. GCC can choose a
different legal evaluation order for nested emitter arguments, changing SSA
numbering and instruction order without changing the C program. The initial
GCC-reference mismatch is retained in the worker logs; the Clang reference and
guest outputs agree for all 71 units.

Example workflow, after building the normal compiler-test image and a matching
ROM/minimal/virtio `BOOT_TEST` platform:

```sh
CC=clang python3 rcc-portable/build.py
python3 rcc-portable/native-sdk/source_os.py \
  --build build/x86_64-boot/riplix-ir-bounded --output build/os-source
python3 rcc-portable/native-sdk/source_os.py \
  --output build/os-source --assess rcc-portable/build/rcc-c

RCC_TEST_ACCEL=kvm python3 rcc-portable/native-sdk/test_os_guest.py \
  --source build/os-source --frontend PATH/TO/GUEST-BUILT/rcc-c.fxe \
  --base PATH/TO/COMPILER-TEST/normal \
  --host-platform build/x86_64-boot/test --output build/guest-os
python3 rcc-portable/native-sdk/test_os_checkpoints.py
```

The compiler-test base must contain matching `kernel.elf`, `boot.img`, `root.img`
and the usual guest Make/mkdir tools. The initial platform profile is checked
against an exact object inventory; another profile requires an explicit update.
The harness reads only regular private disk images and does not install the
result onto a normal user disk or physical machine.

After clean guest shutdown, filesystem checking and extraction, each IR file
must match its fingerprinted host reference. Only then is a native object emitted
and a fresh writable test root created. Boot checks cover process lifetime,
pipes, scripts, timers, writes and execution of a saved application after a
second boot. `fully_guest_built` stays false in the result.

`--reuse-frontend` can retry a later stage using an accepted frontend checkpoint.
It verifies the source manifest, compiler/base/tool identities, full output set
and every IR hash, and checks host parity again. It never turns a partial build
into an accepted source build. Successful guest-build disks are removed after
output acceptance; failures retain their private disks and logs.

## Ubuntu worker

The current worker is `/root/.cache/riplix-build` on the Ubuntu x64 host. Rust
1.97.1 and Cargo caches are private to that directory; Clang/LLD, Make, Node,
QEMU/OVMF and build dependencies are installed through Ubuntu packages. No boot
partition, physical network device or normal Riplix disk is used for tests.
Source `/root/.cache/riplix-build/env.sh` in a build shell to select its private
Rust toolchain, worker build settings and KVM test acceleration.

For the platform acceptance suite on this Ubuntu OVMF build, use the supported
GPT/FAT32 USB image path:

```sh
python3 tools/x86_64/boot.py test --accel kvm --font rom --memory 512 \
  --usb-boot --timeout 180 --output build/ubuntu-os-usb
```

Both persistence boots and all five missing/read-only/read/write/flush-error
rejections pass with that profile. The FAT16 SATA superfloppy boots with the root
present, but its missing-root case stops in firmware before kernel entry on this
host. That firmware/media-profile limitation is not a kernel missing-disk pass.

The recovered 23-unit frontend source build completed its guest Make step in
583.813 seconds under KVM, reusing six verified objects and compiling 17 units.
Its identical-input Mini/TCG duplicate was still running after over three hours
and was stopped after Ubuntu acceptance. The 71-file OS frontend phase took
about two minutes. These are workload observations, not universal KVM speedups.

Evidence is copied into `build/ubuntu-native/`; the remote `logs/` directory
retains build and guest transcripts. The existing accepted SDK/tool generations
remain separate from subsequent source snapshots and generations.

## Remaining work toward a guest-built image

1. Extend generation comparison across the frontend, QBE and assembler together.
   The current 25-unit lowerer/linker/driver/archiver snapshot passes two complete
   source generations with identical executables and execution checks. Frontend,
   QBE and assembler source acceptance pass separately and remain explicit inputs
   to that combined toolchain.
2. Accept the updated portable kernel backend and ELF/EFI/media linker after
   guest source rebuild and actual in-guest OS image generation. Their host
   execution already passes two-boot OS acceptance; see the modes above.
3. Extend generation comparison to the new kernel/image modes and broader
   profiles, keeping the frozen platform capsule and source identities explicit.
4. Initially keep native platform assembly/C and optional Rust math as the
   recorded host objects. Bringing those sources into guest compilation needs
   native layouts/ABI support and additional assembler coverage. It is separate
   from compiling the C16 OS and must not change the active hardware workstream.
5. Move source-generation into guest tools and broaden libc and independent
   application rebuilds. EFI and boot media now have portable implementations;
   their actual guest execution remains its own acceptance gate. Compare
   generations and boot a fresh image before claiming a complete self-hosted OS.

The native LP64/process-memory migration has its own acceptance gates in
[`docs/arch/native-x64.md`](../../docs/arch/native-x64.md). Guest C16 kernel
compilation should remain useful while that work proceeds.
