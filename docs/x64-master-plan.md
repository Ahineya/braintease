# Riplix x64 master plan

Updated 2026-10-22. Direction accepted by the user; individual implementation
slices advance through the gates below.

Riplix x64 is becoming a modern graphical operating system with fast interaction,
consistent applications and a usable laptop environment. Keep shared kernel,
library and application sources with Ripple and Fuse-X where their semantics
fit. Build target adapters and explicit compatibility boundaries as x64 gains
ordinary native processes and hardware support.

The immediate goal is a reliable development machine: persistent storage,
remote access that survives ordinary Wi-Fi key changes, dependable local input,
internal boot and recoverable updates. GUI development and execution foundations
proceed alongside that work. GPU acceleration and multicore execution follow
their own prerequisites rather than delaying the first useful desktop.

## Current baseline

| Area | Accepted work and remaining boundary |
| --- | --- |
| Console | Fast firmware framebuffer console. Physical Escape, four arrows and F1–F12 passed the user's key check. SSH PTY Levee, Commander and Rift checks passed on HP; local framebuffer/cursor display remains pending user acceptance. Fn/media keys remain deferred. |
| Storage | NVMe/GPT with bounded access to dedicated Riplix p3. A 1 GiB C16 V3 filesystem occupies part of the 30 GiB reservation; installed data survives reboot. |
| Networking | AX211 association, WPA2, protected traffic, IPv4/DNS and remote services work on HP. Userland recovery policy exists; native protocol handling and an embedded test profile remain. |
| Key refresh | A real GTK refresh was observed with association generation and connection attempts unchanged, without rejected frames. Periodic snapshots establish reachability afterward; a held SSH session across a rotation remains a separate acceptance gate. Fresh pairwise handshakes still use reconnect fallback. |
| Services and packages | Nine terminal/data packages are published, installed through HP pkg and persisted after reboot. Seven executable/alias hashes, native entry probes and SSH PTY editor/Commander checks passed. Local display acceptance remains pending. Application packages and kernel/EFI updates have different installation paths. |
| Power and boot | The unattended Ubuntu → Riplix → Ubuntu → Riplix loop passed, including automatic Ubuntu root SSH on port 2222. HP is running Riplix; Ubuntu remains the user-authorized temporary default. Shutdown syncs and stops but does not yet power the laptop off. Ubuntu and USB recovery remain available. |
| Execution | Native instructions retain C16 layouts, logical pointers, private calls and checkpoint scheduling at ring 0 on one CPU. The isolated process-memory model has host tests; it does not install production mappings. |
| GUI and audio | Native software GX presentation, screenshots and the LVGL calculator work on HP. User verified touchpad movement and clicks. Multi-window desktop integration and native audio remain ahead. |
| Toolchain | Guest compilation and portable image tools have substantial staged progress. The [bootstrap inventory](../rcc-portable/native-sdk/OS_BOOTSTRAP.md) still distinguishes guest-built stages from the host platform/loader capsule and pending end-to-end acceptance. |

Current device identities come from the [HP inventory](../tools/x86_64/hardware/hp-envy-16-ac0xxx.txt).
Its initial Secure Boot, partition and acceptance descriptions are historical;
the [integration receipts](../tools/x86_64/hardware/hp-integration-plan.txt) and
[x64 implementation notes](../tools/x86_64/README.md) record later progress.
Later monitoring found increased connection generations/attempts; the recovery
log includes prolonged beacon loss and scans temporarily missing the SSID before
successful recovery. Sustained radio reliability therefore remains active work.

## Dependencies and delivery order

Several lanes can progress together. Integration into the running platform and
promotion of physical boot candidates remain coordinated.

| Milestone | Main prerequisites | Completion gate |
| --- | --- | --- |
| Reliable remote development | Current Wi-Fi and services | Held SSH survives a real GTK rotation; traffic, idle operation, fallback recovery and manual pause/resume behave correctly. |
| Dependable terminal applications | Keyboard decoder, console geometry, terminal/PTY contracts | Escape, navigation and F keys work locally; full-screen apps restore terminal state; remote PTYs retain independent sizes. |
| Internal boot and remote updates | Stable NVMe root, verified EFI candidate and recovery | Trial internal boot alongside Ubuntu, then default Riplix; failed update recovery and remote reboot/reconnect tested. |
| Protected and preemptible execution | Process backing, helper privilege boundaries, saved contexts | Faulty apps and syscall-free busy loops leave the shell/kernel usable; lifecycle tests reclaim all resources. |
| Pointer and initial desktop | Input events, a pointer transport, framebuffer/GX backend | Shared GUI apps receive correct input and compose bounded damage without busy polling or interfering with other windows. |
| Audio and laptop controls | HP codec/DSP/amp investigation, DMA and ACPI interfaces | PCM playback under network/UI load, actual power-off, usable battery/lid/backlight controls. |
| Conventional native applications | Versioned LP64/syscall/file-octet contracts, native loader/libc | Static ELF64 application executes at ring 3 and interoperates correctly with existing C16 processes. |
| Native x64 kernel | Accepted native target definitions, audited shared kernel and mixed ABI adapters | Shared kernel builds and runs with byte addressing, LP64 types and ordinary alignment; C16 and native applications retain correct process/file/device behavior. |
| Established replacement filesystem | VFS and octet adapters, selected implementation, recovery tools | Existing-format test volume passes binary/long-name/crash-recovery tests before root migration. |
| Threads and SMP | Stable single-CPU execution, TLS, synchronization, per-CPU/device ownership | Correct concurrency, TLB coordination and lifecycle under load; independently test threads before adding CPUs. |
| Advanced display and Bluetooth | Presentation/buffer contracts; USB/HCI and audio respectively | Native display/rendering and wireless audio pass separate device recovery, lifetime and responsiveness tests. |

## Reliable networking and a recoverable boot loop

Keep connection policy in supervised userland services. The privileged driver
owns firmware, packet transport, DMA and device/key operations. Complete saved
profiles, scan/connect/disconnect/status and address policy through explicit
control interfaces; add the curses selector after the CLI contract is stable.
Integrate the existing DHCP lifecycle with the HP interface, retaining explicit
static configuration for fixtures and recovery. Network failure must leave a
local shell available.

Ordinary GTK refresh should preserve association, IP configuration and transport
sessions. Genuine link loss needs bounded retry/backoff and clear service state.
Current discovery permits at most three total passive scans per startup,
shared between missing-SSID retries and regulatory-profile expansion, over the
existing test channels (1/6/11 and 36/40/44/48). Association still supports only
that 5 GHz subset; this work adds no channels or regulatory permissions.
Fixtures cover newly enabled channels after a profile refresh, configured-target
admission when the AP table is full, and stale scan-completion IDs. General
network selection needs broader validated regulatory/channel discovery.
Correlate physical failures with timestamps, scan masks/counts/drops, raw beacon
counters and polling gaps before changing loss thresholds; these fixtures do
not establish sustained radio reliability. GTK counters reset per association.
Track pairwise renewal, deauthentication, queue exhaustion and driver recovery
separately; restarting services repeatedly is not a substitute for those states.
See [Wi-Fi bring-up](../tools/x86_64/hardware/hp-wifi.txt),
[network lifecycle](networking/reliability.md), [DHCP](networking/dhcp.md),
[services](../ports/services/README.md) and [SSH](../ports/ssh/README.md).

Internal boot uses an independent Riplix EFI directory and firmware entry while
preserving Ubuntu and the USB Linux/Riplix recovery choices. Trial a one-shot
internal boot before making Riplix default. It does not require LP64, a new
filesystem or a finished desktop.

That initial trial and Riplix-default promotion passed. For the current
development stage, the user explicitly authorized a temporary Ubuntu-first
order: Ubuntu entry `0001`, then Riplix entry `0000`, then the existing recovery
entries. Ubuntu's supervised `riplix-dev-ssh.service` automatically provides the
same requested passwordless root access on port 2222; its authentication method
is `none`, not key-only. The original home-directory script is preserved.
A real Ubuntu-to-Ubuntu reboot verified a fresh boot ID, pinned SSH access and
the enabled/active service, recorded in the
[autostart receipt](../build/x86_64-terminal-wave/physical/ubuntu-autostart-accepted.json).

From Ubuntu, verified EFI publication remains a root-run staging operation.
The new terminal candidate has its own `EFI/Riplix-tui` namespace and entry
`0002`, selected once with `BootNext=0002`; entry `0000` remains the previous
accepted Riplix fallback. All eleven pre-existing ESP files and GPT metadata
were preserved, and Ubuntu remains the persistent default. The
[staging receipt](../build/x86_64-terminal-wave32/physical/installed.json)
records that boundary; staging itself did not write the Riplix root partition.
The new EFI booted on HP and reached pinned SSH and running services; three
gateway pings and two DNS-resolved external pings passed, recorded in the
[first-boot receipt](../build/x86_64-terminal-wave32/physical/first-riplix.json)
and [traffic check](../build/x86_64-terminal-wave32/physical/first-riplix-traffic.json).

The full unattended Ubuntu → Riplix → Ubuntu → Riplix round trip has passed:
Ubuntu returned with a fresh boot ID and automatic root SSH on port 2222, and
all nine package versions persisted with Rift's exact hash checked again.
HP is now running Riplix; Ubuntu remains the temporary default. The
[final physical receipt](../build/x86_64-terminal-wave32/physical-acceptance.json)
records this acceptance and the preserved ESP/GPT state. This is
a development loop, with no Riplix ESP writer or automatic health rollback.
The [Ubuntu development-loop procedure](../tools/x86_64/hardware/ubuntu-dev-loop.txt)
records the exact authentication scope, HP-only root-setting restoration,
service removal and boot commands. Each new candidate still requires its own
physical acceptance.

For remote kernel updates, stage and verify a candidate, retain a previous
working image and confirm health after boot. Specify a real recovery mechanism
for a candidate that hangs before confirming health; BootOrder/BootNext alone
does not provide it. Implement ESP access and update publication explicitly.
Keep filesystem/root upgrades separate from EFI rollback: an old EFI using the
current root is not a rollback of installed data.

## Keyboard terminal and application delivery

The first implementation wave fixes Escape, arrows, navigation keys, F1–F12,
modifier state and bounded multi-byte terminal input. Fn may be consumed by
firmware; discover actual scan codes or HP WMI/Intel HID/ACPI events before
promising Fn or media-key support.

Physical Escape, four arrows and all F1–F12 are now user-verified through the
key-check application. Fn/media-key discovery remains deferred. The new console
cursor/ANSI implementation and Levee arrow decoder pass host and exact QEMU
tests. On HP, SSH PTY checks now pass Levee right-arrow/delete/save, Commander
F10 exit and Rift insert/Escape/save with exact file contents. Local framebuffer
and cursor display still need user acceptance. Levee arrows are command
and operator motions. Its legacy insertion buffer deliberately ignores arrows
with a bell rather than inserting or deleting text; full insert navigation is
not implemented. See the [Levee regression receipt](../build/x86_64-levee-input/result.json).

Report the physical console's real grid through the terminal-size device ABI
and TIOCGWINSZ. Check ANSI cursor/erase/scroll behavior, termios, PTY resize and
restoration after signals or exit. Levee's own size limits are distinct from
platform geometry. These fixes improve the existing shell/application stack.

Bring [mux](terminal-multiplexer.md), Commander with its `mc` alias and
[Rift](../rfl/apps/rift/README.md) first. mux is a terminal application and has no
GPU dependency. Build against the matching x64 C16 SDK and supply resources,
PTY/job-control behavior and package dependencies. Reuse the
[retained-rendering improvements](rfl/commander-rift-performance.md).

The current terminal set contains `mux`, Commander (`rfl-fm`/`mc`), `rift`/`vi`,
`keycheck`, fixed `levee`, and four data/license packages: nine archives total.
Private package transactions preserve user configuration, and the managed
package payloads pass exact QEMU execution. The RXE1 limit is now 32 MiB for
the entire package, including metadata, alignment and code; boundary, rollback
and reclamation checks pass. This accommodates the current Rift build.
[Candidate acceptance](../build/x86_64-terminal-wave32/acceptance.json) and the
[managed TUI execution receipt](../build/x86_64-tui-wave/qemu-x86_64-terminal-wave32-managed-c11f593ddbfa/result.json)
record Commander navigation/F10, Rift editing/save, Levee motion/save, 16 PTYs
and mux tabs/detach/cleanup.

That nine-package subset has now been
[published](../build/x86_64-tui-wave/publication.json) while retaining all 481
catalog entries, and installed through the HP's actual `pkg` commands.
The [HP installation receipt](../build/x86_64-terminal-wave32/physical/packages-installed.json)
records seven exact executable/alias hashes (`levee`, `keycheck`, `mux`,
`rfl-fm`, `mc`, `rift`, `vi`), native entry probes and post-install network health.
The [final physical receipt](../build/x86_64-terminal-wave32/physical-acceptance.json)
adds package persistence after the unattended round trip and successful SSH PTY
Levee motion/save, Commander F10 exit and Rift insertion/save checks. Local
framebuffer/cursor display acceptance remains pending.

Then bring chat, Telegram and the remaining collection as their dependencies
become available. [Telegram](../ports/telegram/README.md) uses crypto services
that need an HP implementation; a working socket does not supply them. Music
and wavplay need the PCM backend. Existing hosted GUI executables are not
freestanding HP packages. Keep the user's [package catalog](packages/README.md)
and [pkg implementation](../ports/pkg/README.md) as the delivery mechanism,
installing useful groups incrementally.

## Memory isolation preemption and native applications

The [native x64 architecture](arch/native-x64.md) governs execution and ABI
details. Keep one kernel, process table and scheduler throughout migration.

1. Measure current switch-copy work, scheduling gaps and long protected runtime
   operations. Integrate reviewed process-owned hardware backing under the
   existing safe checkpoints, with explicit allocator/PTE/TLB/rollback and
   borrowed-view lifetimes. The [isolated model](../tools/x86_64/process-memory/DESIGN.md)
   is a starting point for this interface, not an activated address space.
2. Separate user-compatible helpers from privileged services. Introduce ring-3
   mappings, permissions, private stacks, saved CPU/FP state, checked user copies
   and process-local fault cleanup. Existing packages need an explicit safety
   transition; machine-code generation alone does not make them ring-3 safe.
3. Add timer-driven single-CPU userspace preemption. Initially keep kernel
   critical operations nonpreemptible and bounded. Exercise a syscall-free busy
   loop beside an interactive shell, mixed I/O, fork/exec/signals and resource
   reclamation. Do not switch current backing beneath borrowed pointers or
   retain contexts on a shared interrupt stack.
4. Add versioned Riplix static ELF64 applications using System V AMD64 LP64,
   ordinary byte addressing and alignment. Define layouts, relocations, entry
   stack, syscalls and file-octet/C16-cell conversion. Use Clang as the initial
   oracle, supply a native SDK and extend guest RCC/RFL targets afterward.
   Preserve C16 execution through adapters. ELF is not Linux binary compatibility.

LP64 design and compiler work can proceed beside C16 isolation. Hosting the Rust-based RFL
compiler also needs selected Rust platform/library services; compiling C16 code
in the guest does not establish that support.

## Native x64 kernel destination

The end state includes a conventional native x64 kernel as well as native
applications. Today's native hardware C/assembly surrounds a shared kernel
compiled with C16 semantics. Migrate that shared kernel to byte addressing,
LP64 types and ordinary C alignment, keeping reusable kernel sources and target
definitions rather than maintaining an unrelated replacement kernel.

Begin with a Clang cross-build after the native interfaces and mixed-process
contracts are accepted. Audit integer widths, pointer arithmetic, structure
layouts, allocation sizes, syscall marshalling and hardware/user copies. Keep
on-disk and device layouts explicitly encoded. Maintain the C16 application ABI
through compatibility adapters while kernel services move to native
representations. Run process, signal, descriptor, terminal, storage, network and
shutdown regressions with both application families before retiring conversions.

Extend RCC and the guest toolchain to rebuild the native kernel and its platform
components, with every remaining host bootstrap input recorded. A native kernel
build, guest self-hosting and SMP have distinct acceptance gates. The earlier
memory-isolation/preemption work does not need to wait for this migration.
See the [full native migration stages](arch/native-x64.md#full-native-migration-stages).

## Input devices GUI and graphics

Use one input contract for key/modifier state, pointer motion/buttons/wheel,
focus and capture. Adapt it to [terminal mouse reports and GX events](graphics/architecture.md),
keeping applications above device transports.

HP has an i8042 keyboard, a Synaptics HID-over-I2C touchpad on Intel
LPSS/DesignWare, and an ELAN HID-over-I2C touchscreen/stylus. PS/2 AUX is disabled.
External USB mice need xHCI, enumeration and HID. Develop the necessary
ACPI/resource/interrupt and transport support in bounded modules, sharing HID
decoding where appropriate. Touchscreen gestures, calibration, rotation and
tablet mode follow the first dependable pointer path.

Start the HP GUI with software rendering and bounded damage presentation over
the firmware framebuffer. Preserve shared surface formats, window lifetimes,
input, clipboard and presentation contracts while the user develops LVGL.
Reuse the [LVGL work](graphics/lvgl-and-x11.md),
[graphics roadmap](graphics/roadmap.md), [FuseDE plan](fusede/implementation-plan.md)
and [Fuse-X contracts](fuse-x/roadmap.md). Their hosted/RVM backends are reusable
reference implementations; the HP needs its own backend.

A responsive multi-app desktop requires bounded rendering and execution
isolation/preemption. Native Intel graphics has distinct display/modesetting,
backlight and acceleration stages. For the Meteor Lake GPU, establish buffer
ownership, presentation, memory mapping and reset/fault handling before GPU
command submission. Keep the same surface API and measure software composition
before selecting acceleration work.

## Audio power and Bluetooth

Reuse the [PCM contract](arch/audio.md), including configure/write/readiness,
drain and ownership. Investigate HP's Intel SOF/HDA controller, codecs, firmware
and cs35l41 amplifiers before selecting a first output path. Test DMA lifetime,
partial writes, queue starvation, underruns and playback while SSH, storage and
UI work run. A generic HDA implementation alone does not establish speaker output.

Extend working reset with actual ACPI S5 power-off, battery/AC, lid, backlight
and measured idle/thermal behavior. Suspend/resume requires coordinated
quiescence, DMA safety, device restoration and network recovery.

HP Bluetooth is a USB device independent of AX211 Wi-Fi. Headphones require
USB/HCI/firmware support, discovery/bonding, an audio profile and codec/routing/
buffering integration. Build it after basic USB and PCM; retain wired output
while validating the additional wireless lifecycle.

## Established filesystem adoption

Adopt an existing filesystem and reuse a suitable existing implementation rather
than inventing another on-disk format. The format is not selected yet. Compare
recovery/fsck tooling, journaling or other crash-consistency guarantees, license,
implementation dependencies and realistic porting effort before choosing.
The [filesystem adoption assessment](arch/x64-filesystem-adoption.md) compares
ext2, ext4 and existing code candidates,
including the device flush and recovery contracts needed before writable use.

The [C16 V3 format](arch/c16-filesystem.md) has fixed 30-character names, cell-based
contents and no journal. Define VFS/mount boundaries, byte-oriented disk adapters,
filename encoding, links/permissions and flush ordering. Share octet/C16
conversion semantics with the native syscall work. Extending NAME_MAX alone
cannot change the existing directory encoding.

Bring the selected backend up on disposable images and a separate test volume:
malformed-media rejection and read-only access first, then writes and crash
recovery with injected failures and independent checking. Add installer,
recovery and migration tooling before promoting it to root. Long names, exact
binary contents, large files and power-loss recovery are explicit gates. Keep
Ubuntu and unrelated partitions outside all Riplix write windows.

## Threads and multicore execution

Start threads on one CPU after execution, TLS and synchronization contracts are
defined. SMP follows stable scheduling/isolation, kernel ownership and locking,
per-CPU stacks and FP state, AP startup, TLB shootdowns and interrupt/device
concurrency. The HP has 12 cores and 14 logical CPUs; enabling those CPUs must
not allow concurrent entry into today's unprotected global runtime.

## Parallel work and ownership

| Lane | Independent work | Shared integration boundary |
| --- | --- | --- |
| GUI and LVGL | User-owned controls, themes, applications and hosted rendering | GX/input/presentation contract and HP backend |
| Applications and packages | Matching x64 builds, resources, manifests and app tests | Capability availability, SDK and executable identity |
| Keyboard and terminal | Decoder, sequence/geometry fixtures and console adapters | Platform input/device ABI; parent integrates runtime/build changes |
| Execution | Process-memory model, adapter contract, bounded mapping/lifetime tests | Platform allocator/PTE/runtime/IRQ hooks and existing scheduler |
| Hardware | Explicitly scoped USB/I2C/HID/audio modules and protocol fixtures | Shared PCI, ACPI, IRQ, DMA and input interfaces |
| Native ABI and storage | ABI probes/SDK design and existing-filesystem evaluation/tooling | File octets, shared kernel headers, loader tags and mount interfaces |
| Platform integration | Combined QEMU acceptance, EFI candidate, HP testing and recovery | One integrating owner and one physical candidate at a time |

Keep the existing process-memory delegation limited to
`tools/x86_64/process-memory/`; a production adapter needs a separate explicit
file/API agreement. Coordinate reserved compiler sources and shared kernel/SDK
changes through the [workstream note](arch/native-memory-coordination.txt).
New task scopes should name files and interfaces before concurrent edits.

Before new hardware milestones, inspect Linux and OpenBSD support for the exact
device/revision. Pin the sources used for protocol structures and ordering.
Prefer porting proven driver cores where licensing and dependencies fit; supply Riplix
DMA/IRQ/timer/synchronization adapters explicitly. Avoid an accidental general
Linux compatibility layer or guesses about packed device structures.

## First implementation wave

Started 2026-10-21:

- Keyboard agent: pure host-testable decoder, i8042 input integration and focused
  tests. No shared runtime/compiler or LVGL edits.
- Terminal agent: real console dimensions, validated device descriptor helper
  and bounded ANSI/color/line-drawing fixes for shared curses output. Parent
  owns runtime and build integration.
- Execution agent: [concrete process integration contract](arch/x64-process-integration.md)
  grounded in the current model and production code. No production mapping or
  scheduler changes in this slice.
- Wi-Fi follow-up: bounded passive discovery retries, rescanning channels newly
  enabled by the regulatory profile, target admission under AP-table saturation,
  and diagnostic-only beacon/RX/polling context. No new bands, active probes or
  changed beacon-loss thresholds in this slice.
- Platform owner: master documentation, combined build, appropriate QEMU checks
  and exact candidate receipt, followed by USB refresh and internal EFI staging.
  Preserve Ubuntu, installed Riplix data and recovery choices.

The first combined radio/console candidate was accepted by host tests and exact
QEMU checks: 23 Wi-Fi suites plus virtio/mailbox/Python checks (26 checks in the
receipt), fatal-UBSan keyboard/adapter, geometry and ANSI tests, two persistence
boots and actual firmware reset. QMP/i8042 input produces exact
Escape/Up/F1/F5/F12 octets through the TTY and shell. Under QEMU, console
TIOCGWINSZ reports the renderer's 104-column by 28-row grid. GPT metadata and
named outside-partition canaries remain unchanged. The original
[candidate receipt](../build/x86_64-radio-console/acceptance.json) identifies
EFI `dab393f9ca5aa4fcd2a616e16b1e4868f0f9047bd389492c7cf437bc681f4147`
and retains the physical-acceptance boundary.

Discovery now permits at most three total passive scans per startup, shared
between profile-expansion rescans and missing-SSID retries. Distinct scan IDs,
stale-completion guards and command-queue headroom keep recovery bounded;
configured targets can enter a table already filled by neighbouring APs.
Live diagnostic fields record beacon-loss context without changing loss policy.

USB boot files have been refreshed and read back, with all recovery choices
retained. All native Wi-Fi objects were rebuilt for the changed private state;
the accepted compiled C16 kernel/apps remain the compatibility baseline, and
the installer did not rewrite the Riplix root partition. Internal EFI staging
and its USB-free HP trial passed: pinned SSH, services, gateway traffic and
DNS-resolved traffic worked with generation1/attempt1. Riplix was then promoted
to the default; another USB-free boot and its remote/network checks passed.
Orderly return to Ubuntu and its original entry were verified. The
[physical receipt](../build/x86_64-internal-boot/physical-acceptance.json) records
that result. Physical key acceptance and the next terminal candidate are
recorded below; sustained-radio acceptance remains pending.

The subsequent terminal candidate adds cursor/ANSI fixes and the 32 MiB total
RXE1 limit. Four console pixel suites, loader-boundary/rollback/reclamation
checks, two exact QEMU persistence boots with firmware reset, and the nine
managed terminal/data packages have passed. Candidate EFI
`32d71c89df9ed6ed8e0a039faa19b931d1a1ef529770c8a64a70ad6fc5790691`
is identified by the [current acceptance receipt](../build/x86_64-terminal-wave32/acceptance.json).
Its separate `EFI/Riplix-tui` entry `0002` has since booted on HP with pinned
SSH, services and gateway/DNS traffic. Nine packages are published and installed,
with seven exact executable/alias hashes and native entry probes verified.
The unattended Ubuntu → Riplix → Ubuntu → Riplix round trip and SSH PTY Levee,
Commander and Rift checks have since passed, as recorded in the
[final physical receipt](../build/x86_64-terminal-wave32/physical-acceptance.json).
All nine package versions persisted; Rift's hash was rechecked after reboot.
Local framebuffer/cursor display still needs user acceptance. The user's
separate physical key check passed Escape, all four arrows and F1–F12; Fn/media
keys are deferred. Ubuntu remains the temporary default and entry `0000` the
previous accepted Riplix fallback. SSH autostart and staging/installation
receipts are linked above.
Broader Unicode/font coverage and each full-screen application's real display,
terminal restoration and input behavior retain their own acceptance gates.

The isolated real-allocator/inactive-PTE fixture has now passed on the HP's KVM:
allocation failures roll back, mappings remain inactive, and final page/image
accounting is balanced. This does not install process page tables or enable
ring 3/preemption. See the [fixture receipt](../build/x86_64-process-memory-fixture/hp-kvm-result.json).
Filesystem work still starts with its disposable host-image experiment.

The next graphics slice implements the existing GX interface over native GOP
presentation, keyboard events, display ownership and normal Alt+F4 exit. The
existing LVGL calculator reuses 230 cached units. Its focused KVM trial passed
calculator input, console restoration, cleanup and byte-exact screenshot parity;
the user subsequently confirmed its display, keyboard input and close action on
the HP. `lvcalc`, `screenshot` and `touchpad` are now signed x64 packages in the
existing local repository. See the [backend notes](../tools/x86_64/graphics/README.md)
and [publication receipt](../build/x86_64-graphics/catalog/publication.json).

Native screenshots and a reduced preview have been retrieved from the real HP.
Captures are row-wise; they do not guarantee an atomic moving frame. This is a
single-active-surface backend, not the complete FuseDE compositor. The user's
LVGL/FuseDE/SDL work remains independent.

Touchpad controller/HID initialization, physical movement and clicks now pass on
the HP; the user confirmed movement and calculator button clicks. Native diagnostics found the
I2C controller's BAR unassigned before and after D0; Linux's PCI resource setup
had supplied the address seen during Ubuntu discovery. The original combined
"BAR alignment" error was misleading. The new path snapshots firmware PCI
apertures and occupied ranges before ExitBootServices and assigns only the
unassigned target resource. Its protocol/parser, range-selection and BAR
rollback tests pass. Early native BAR measurements corrected the GPU/audio
fallback reservations that had consumed the whole aperture; the controller now
reclaims its cached firmware resource and reports ready with zero I2C aborts.
See the [input integration notes](../tools/x86_64/input/README.md).
After repeated failures, the complete Linux/OpenBSD/UEFI comparison now covers
the whole attachment path in one batch: optional PCI metadata, D0/BAR recovery,
LPSS reset/remap, dynamic optional GPIO, DesignWare completion and HID reset,
features and report recovery. Focused sanitizer tests and native compilation
pass. Candidate `4444d9166e54be07dfa2e0ff54512f52375f42018e3857917db5aca17ccd171c`
has booted with SSH/Wi-Fi/services healthy. The user confirmed physical pointer
movement and calculator clicks; gestures, scrolling and additional pointer
transports remain separate work.
All updates use the internal EFI/SSH loop with Ubuntu as the development default;
the USB remains recovery media. Audio, ring 3 and filesystem durability retain
their separate target tests.

## Performance and acceptance records

Track boot-to-shell and remote-ready time, input-to-paint latency, frame damage
and copied bytes, process switch work, memory reclamation, CPU wakeups and audio
underruns. Compare idle operation with realistic simultaneous applications.
Use these results to choose optimization work and resource budgets.

Keep small source/hash/test/install receipts beside implementation work. Prefer
host tests and disposable QEMU images before physical promotion. Keep a working
EFI recovery path and avoid repeated full-root exports or large build backups.
The [older HP integration order and dated receipts](../tools/x86_64/hardware/hp-integration-plan.txt)
remain the operational history; this master plan organizes subsequent work.
