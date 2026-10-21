# Riplix on x86_64

Riplix boots as freestanding x86_64 code in QEMU: the shared kernel mounts a
writable virtual disk root, runs `/init` as PID 1 and loads the existing V7 Bourne shell
from `/bin/sh`. Both virtio and NVMe virtual disks pass persistent-root acceptance. The QEMU window accepts a polled PS/2 keyboard. Independent
x86_64 apps, shell scripts, pipelines, redirection, command substitution, fork,
private copy-on-write memory, exec, wait, timers and file I/O pass acceptance.
The PC timer delivers real hardware interrupts; deferred scheduling runs at
safe application checkpoints, and idle waits use `halt` until an interrupt.
The hardware target is an HP Envy x360 16-ac0xxx. Its
[read-only hardware inventory and boot plan](hardware/hp-envy-16-ac0xxx.txt)
records a Core Ultra 7 155U, 16 GB RAM, Intel graphics, a PS/2 built-in keyboard,
I2C touch devices and NVMe storage. High framebuffer mappings and ACPI/APIC
initialization now pass QEMU checks, including remote KVM/x2APIC on the HP.
The prepared USB has now booted directly on the HP: the user's photographs show
the shell and passing app/process/pipe/timer checks with an embedded RAM root.
The user disabled Secure Boot for this unsigned image and subsequently confirmed
shell scripts/checks and a physically fast console on the refreshed build.
The read-only NVMe/GPT probe also passed on the HP: Identify, both GPT copies,
expected partitions and the RAM-root shell are photographed. The dedicated
30GiB NVMe reservation subsequently passed physical two-boot file/executable
persistence and orderly shutdown; Ubuntu also boots normally afterward. See the
[USB handoff](hardware/hp-usb-boot.txt).

Riplix applications initially retain the existing C16 layout and private native
ABI. Native x86_64 instructions do not require changing C cells or logical
pointers. Hardware addresses belong to a platform layer with real 64-bit
pointers. An ordinary host C ABI and a new application data model are separate
projects.

The accepted [native x64 migration plan](../../docs/arch/native-x64.md) adds
conventional LP64 applications beside this compatibility path, establishes
native memory/context switching and preemption, then migrates the shared kernel
and compilers. It also separates hosting the Rust-based RFL compiler from the
ABI it emits. These are planned stages; current package and runtime contracts
remain unchanged.

The freestanding build explicitly selects the `bounded` heap profile for both
kernel and SDK/application compilation: banks 48..239. Its current runtime
backs only 256 banks. The hosted profile uses sparse high banks and remains
the hosted default. `boot.py` selects the bounded profile automatically and
uses separate caches; no extra launch option is needed. Building freestanding
apps against the hosted heap would halt on an unsupported bank-256 access.

## Unified HP system candidate

The [integration plan and current handoff](hardware/hp-integration-plan.txt)
records the agreed route to automatic networking/services, packages and internal
dual boot. The prepared default USB now combines AX211 with the dedicated NVMe
root, supervised IP/Telnet/SSH services and the signed Mac mini pkg bootstrap.
Its filesystem is1GiB with8192 inodes inside the unchanged30GiB reservation.
Physical combined networking, package installation, pinned SSH/SFTP and three
forced radio-stop recoveries have passed. Recovery remains on the USB;
internal EFI/NVRAM installation has not happened yet.

`--apps system --system-services --root-mib 1024 --root-source OLD_ROOT`
installs the small integration set while retaining existing clean-root apps.
It requires persistent networking. Add paired `--ssh-tools DIR` and
`--ssh-authorized-key PUBLIC_KEY` to seed static RXE1 SSH tools and public
client authorization; the guest generates its private host key on first boot.
The x64 entropy mailbox uses checked CPU randomness with bounded retries and
failure-atomic output; unsupported hardware fails rather than using the clock.

The current bootstrap at `/root/pkg-setup/install.sh` installs `pkg` and `test`
from the pinned mini repository. Use `PATH=/usr/bin:/bin; export PATH` in existing
sessions. Packages remain `riplix-x64-c16`; Fuse-X/RFL apps need separate x64
ports. `wifi status`, `wifi down` (pause recovery), and `wifi reconnect` (resume)
are provided by the system-services profile. Supervised `wifid` retries failed
startup/link loss with delays of 2,4,8,16,32,60 seconds and runs the explicit
`/etc/wifi-online.sh` address/service hook after recovery. Native retry telemetry
uses serial; inspect `/var/run/svcd/wifi.log` for state, reasons and backoff.
Initial association still runs synchronously in the native driver with an
embedded development profile. Full reconnect can interrupt sessions. Live
GTK refresh now runs asynchronously for RSN descriptor2 with CCMP/TKIP
group keys. It authenticates and installs a new GTK within the existing
association, retains overlapping key IDs and per-key replay counters, and
acknowledges retries without reinstalling keys. `wifi status` reports refresh,
duplicate and rejection counters. Host tests cover 1,200 synthetic rotations,
queue wrap/reuse and failure handling; real router rotation and SSH continuity
still require physical acceptance. A new pairwise four-way handshake retains
the reconnect fallback. Selectable profiles, full userspace authentication,
DHCP, internal default boot and verified kernel updates remain subsequent stages.

`reboot` requests existing orderly service shutdown/filesystem sync, then a
firmware-described FADT reset after native cleanup. QEMU reset passes; the user
also confirmed HP reboot after the late MMIO ownership fix. Ordinary
`shutdown` still syncs and halts; it does not yet power off the HP. Fatal kernel
diagnostics display `[KERNEL PANIC]` with the reason and numeric detail; serial
`BOOT_FAIL` markers remain available to acceptance tools.

Late device mappings retain page tables in the kernel CR3 and cannot be freed
as context allocations. The scheduler's cleanup balance includes only these
explicit reservations, retaining its real-leak check. To reproduce the HP
two-page false positive, verify repeated mapping reuse, check rejection of an
intentional real leak and exercise the corrected reset path in disposable QEMU:

```sh
python3 tools/x86_64/page_ownership_check.py \
  --accepted build/x86_64-wifi-recovery-v3/normal \
  --capture build/x86_64-hp-persistent/gpt-after.json \
  --identity build/x86_64-hp-persistent/root-identity.json \
  --output build/x86_64-page-ownership
```

This uses accepted compiled apps and synthetic disks, never a physical device.
QEMU does not emulate the AX211 radio or certify physical HP firmware reset.

## Offline CLI collection

`--apps offline` builds 105 existing CLI commands plus four diagnostic tools
as static checked/bounded RXE1 packages. It requires persistent root; the tiny
`minimal` profile remains the default for bootstrap/RAM tests. GUI/network
clients, the separate RFL editor packaging and the graphics-dependent multiplexer
are excluded explicitly in `application-report.json`. Building the collection
is strict: a failed app removes its stale success/manifest and fails the build.
Interactive editors/games still need their own UI acceptance.

```sh
python3 tools/x86_64/boot.py build --apps offline --storage nvme --usb-boot \
  --nvme-partition --root-identity /path/to/verified-root-identity.json \
  --output build/x86_64-offline
```

The offline seed is 128MiB with manuals, BASIC examples, Sokoban levels, terminal
configuration, make support files and `/offline-check.sh`. The normal launcher
preserves an existing disk; it does not upgrade it automatically. The owned root
installer can grow a BACKED-UP regular C16 image with `--offline --upgrade OLD`;
verify filesystem consistency and all unmanaged file cells before installing it
into an independently verified, unmounted dedicated partition. This is not an
Ubuntu partition resizing operation and does not change the 30GiB reservation.

`sh /offline-check.sh` exercises system/text/file commands, awk including native
math, BASIC, Tcl/Picol, m4, dc, make, ed, tar, manuals and command wrappers. Require
the actual final `OFFLINE_APPS_OK` line. Compilation is not full runtime coverage
of every command/option. Production acceptance also runs the existing process,
pipe, timer, failed-exec and copied-app persistence tests across two QEMU boots.

Native SSE2 and freestanding libm supply the existing math helper; see
[the native math build/contract](math/README.md). The compiler's C16 data/call
layout remains unchanged. A local generated awk copy uses `return 1.0` for its
power base case, tied to expected-correct RED rct regression
`test_native_recursive_double_power`; the shared compiler/source is untouched.

## Networking and the HP Wi-Fi target

Current live-interface build, controls, acceptance and limits: [live Wi-Fi](hardware/hp-wifi-live.txt).
The individual stopped checkpoints below retain their earlier regression roles.

The HP target is its **built-in Intel AX211 Wi-Fi**. QEMU's virtual Ethernet
card is a way to accept the shared TCP/IP stack and remote shells without a
cable. Native AX211 firmware initialization, NVM readback and dual-band passive scans
have passed on the HP, including the user-confirmed 5 GHz AP. Initial 802.11
authentication and association now pass physically (AID 10, all three firmware
updates). The WPA2 key handshake and protected unicast ARP have also passed physically.
The next experimental profile keeps the radio alive and attaches it to the existing
IP stack; see the [live Wi-Fi handoff](hardware/hp-wifi-live.txt). Sustained physical
IP/Telnet acceptance remains pending. The working recovery image has
been inspected: it uses Linux iwlwifi, the matching firmware family,
NetworkManager/nmtui and wpa_supplicant. See [the Wi-Fi findings and driver
gates](hardware/hp-wifi.txt). A similar curses setup UI is planned after the
radio-control path works; no password is needed for discovery.

```sh
# Two private NVMe-root boots, real host traffic, remote PTYs and link recovery.
python3 tools/x86_64/boot.py test --network user --storage nvme \
  --output build/x86_64-network --timeout 360

# Interactive QEMU. A new dedicated disk receives the network app seed.
python3 tools/x86_64/boot.py run --network user --output build/x86_64-network-run
# At the guest console: sh /network.sh
# Connect from the Mac: telnet 127.0.0.1 2323

# Optional read-only HP PCI/CSR probe; does not initialize Wi-Fi.
python3 tools/x86_64/boot.py build --ram-root --wifi-probe \
  --output build/x86_64-wifi-probe
```

An experimental `--wifi-firmware PATH` profile now packages the pinned recovery
AX211 firmware and tests only startup/ALIVE, then stops the device before the
shell. It requires `--ram-root` and cannot be combined with a disk/NVMe probe
or QEMU networking. It has native DMA/PCI guards and host tests; **physical
ALIVE, initialization, NVM, dual-band passive scans, authentication and association
have passed on the HP, followed by WPA2 and protected unicast ARP. Live IP
acceptance is pending**. Bounded checkpoint profiles stop; the explicit
`--network wifi` profile transfers the running radio to the NIC adapter instead. The optional USB entry
is `EFI/BOOT/BOOTWIFI.EFI`; select it through the HP's **Boot From EFI File**
menu. The default USB entry remains the accepted offline system. See the
[exact candidate, test results and hardware steps](hardware/hp-wifi.txt).

```sh
python3 tools/x86_64/wifi_check.py \
  --firmware build/x86_64-network/wifi-reference/iwlwifi-ma-b0-gf-a0-89.ucode \
  --pnvm build/x86_64-network/wifi-reference/iwlwifi-ma-b0-gf-a0.pnvm
# On this Mac's toolchain, add:
# --sysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
```

The HP has physically passed firmware ALIVE, PNVM notification, initialization,
checked NVM readback, controller stop and return to the RAM-root shell. Add
`--wifi-pnvm build/x86_64-network/wifi-reference/iwlwifi-ma-b0-gf-a0.pnvm`
to the firmware build above for this checkpoint. PNVM status `0x50001` stays
opaque; neither Linux nor OpenBSD requires zero. The board returned version
`0x1290`, 110 channel entries, 38 marked valid and TX/RX chain masks 3.

The next optional test adds `--wifi-scan`. It reads the actual strap/OTP MAC,
configures the matching integrated MA radio, queries the current/default
regulatory profile and scans allowed channels 1, 6, 11 and 36, 40, 44, 48
passively. No directed SSIDs, active probes, association or data traffic. It keeps up to 32 distinct
BSSID/channel entries and displays escaped SSID text, signal and privacy/RSN
presence after stopping. A completed empty scan is reported as zero APs; it is
not a timeout or evidence that the full band is empty. RX ownership remains
bounded to one ring pass; exhaustion stops explicitly. Physical 2.4/5 GHz discovery
and deferred country-profile refresh are accepted: 13 APs including the target
on channel 48. Country notifications are validated and followed by a bounded
MCC update carrying the exact detected country/source, following Linux's CHUB
handler. The initial query remains ZZ/GET_CURRENT; returned flags alone govern
channel permission. Results on newly prohibited channels are removed.
`BOOTWIFI.EFI` is the candidate; `BOOTW24.EFI` retains the accepted 2.4 GHz scan,
`BOOTWNVM.EFI` retains the accepted NVM test and `BOOTWALV.EFI` retains the accepted ALIVE-only image. See the
[current hardware handoff](hardware/hp-wifi.txt).

```sh
python3 tools/x86_64/boot.py build --ram-root --wifi-firmware \
  build/x86_64-network/wifi-reference/iwlwifi-ma-b0-gf-a0-89.ucode \
  --wifi-pnvm build/x86_64-network/wifi-reference/iwlwifi-ma-b0-gf-a0.pnvm \
  --wifi-scan --output build/x86_64-wifi-start-shell
```

An optional `--wifi-prepare-ssid 'YOUR SSID'` after `--wifi-scan` tests the next
station-setup gate. It parses bounded RSN/rate/beacon information, selects the
strongest compatible exact SSID, and requires WPA2-PSK with CCMP unicast,
supported basic rates and no mandatory management-frame protection. It recognizes
CCMP or TKIP group advertisements for this no-transmit setup test only; GTK/TKIP
group encryption remains unimplemented. It rejects unsupported
security explicitly without choosing an open/WPA1 alternative. Only permitted
low-band 5 GHz/20 MHz is supported at this checkpoint. The selected BSSID and
security snapshot are frozen before setup; new country changes stop the test.

Seven matched firmware commands create PHY/RLC/MAC/link/station contexts and a
native management TX queue. No authentication frames or data are transmitted,
no keys/passwords are used, and the device is stopped before the shell. Success
is `AX211_STATION_SETUP_REPLIES_OK`; actual queue TX is still unverified.
The initial CCMP-group-only test stopped before station setup on the user's AP.
A retry prints `AX211_TARGET_SECURITY` and its raw group suite beside the final
outcome, after verbose diagnostics, so its exact cipher remains visible.
Host UBSan covers 112 setup scenarios plus the earlier scan/startup regressions;
wire sizes/offsets also match extracted pinned OpenBSD declarations. Use
`tests/wifi-prepare.c` with the same native source list and firmware arguments
to run the combined host regression. The current USB/hardware instructions and
accepted scan fallback are recorded in [the hardware handoff](hardware/hp-wifi.txt).

The HP subsequently reached all seven station commands and returned queue 1,
initial write pointer 0, raw flags 1. The actual target is now confirmed to
advertise TKIP group traffic with CCMP pairwise and PSK. The old zero-flags
check rejected that reply. Matching Linux/OpenBSD allocation consumers do not
use this legacy flags field; the driver now retains it raw while keeping the
reply-size, command-tag, queue-ID and initial-pointer guards. Queue padding is
also retained raw, as both allocation consumers ignore it.
A reply alone does not establish that the queue transmits.

`--wifi-auth` after `--wifi-prepare-ssid 'YOUR SSID'` explicitly enables one
bounded 802.11 Open System authentication request to the selected WPA2 AP.
The preparation-only profile still transmits no frames. The auth test requires
a discovered BSS on a valid current MCC/SKU channel, a non-radar low-band
channel, antenna A, a
matched TX/session ABI and successful firmware channel-protection start. It
uses the AP's lowest supported basic OFDM rate. A single native Gen3/TFH
frame descriptor is submitted; no host retry or ring reuse is attempted.
`AX211_AUTH_OK` requires BOTH a successful single-frame TX completion matched by payload queue/SSN
and the matching AP's authentication response. Addresses, channel, CRC,
algorithm/transaction/status, RX frame padding and DMA ownership are checked. The whole
exchange is bounded and all paths stop/reset the device before the shell.
This is a preliminary radio exchange, not association or WPA2 authentication:
no password, EAPOL, keys, GTK/TKIP encryption or usable IP link yet.

The first physical auth candidate stopped at the channel-permission gate,
before a session or auth frame was submitted. It exposed a missing CHUB apply
step: querying ZZ again does not apply the detected country. The corrected
candidate forwards firmware's country/source and retains the returned actual
country/source and target MCC/NVM flags beside the final outcome. Hardware now
confirms AL/source5 was applied, but flags0xf61 still tripped our extra ACTIVE
check. Upstream ACTIVE is active-scan permission, not a client-auth prohibition.
The source audit removes that requirement for an already discovered BSS;
disabled/radar channels still stop. The user subsequently confirmed AUTH_OK on the consumer-audited build.
An early country notification remains pending across the initial GET_CURRENT
query; a query alone cannot mark that update applied. Host scan tests cover
this ordering, update races, firmware returning a different/world country,
channel removal and malformed country/source fields. Firmware response bytes
13..15 are retained as opaque diagnostics, matching Linux/OpenBSD consumers;
API6 returns nonzero values there, so they are not required to be zero.

Host UBSan covers 42 authentication/session/TX/AP/permission scenarios plus
112 preparation cases and the earlier regressions. The user confirmed physical
initial authentication success on that audited build. The optional `BOOTWIFI.EFI`
now extends this path with the explicit association profile below; the accepted passive scan remains `BOOTW5G.EFI`. Final
`AX211_QUEUE_REPLY` and `AX211_AUTH_RESULT` lines distinguish raw queue flags,
verified TX and the AP response. See the [hardware handoff](hardware/hp-wifi.txt)
for exact candidate hashes and expected markers. The [consumer-check audit](hardware/hp-wifi-checks.txt)
records upstream functions, protocol requirements and deliberate bootstrap limits.
Ten focused compatibility cases cover opaque RX metadata, NVM informational
fields, command ACK payloads, MCC status handling and AX210 TX response queue/
16-bit SSN semantics. Nine reproduced unsupported rejections before correction.

`--wifi-assoc` after `--wifi-auth` adds one legacy 5 GHz association request.
It selects the actual AP group cipher, CCMP pairwise and PSK in the RSN IE,
advertises only supported legacy rates, and requires both positive TX completion
and a matching successful AP response. LINK, STA and MAC are then updated in
OpenBSD's order with the AP-assigned AID. No WMM/HT/PMF features are advertised.
The final marker is `AX211_ASSOC_OK`; the device is stopped before the RAM shell.
This is a bounded association checkpoint, still without WPA2/EAPOL keys or IP.
Physical association is confirmed by the user photo: TX success, AP status 0,
AID 10, all three updates and clean controller stop. Host UBSan passes 92 additional
transport cases plus bounded RX truncations; extracted upstream declarations
verify field sizes/offsets. The current candidate and fallback hashes are in the
[hardware handoff](hardware/hp-wifi.txt), with source functions and explicit limits
in the [check audit](hardware/hp-wifi-checks.txt).

`--wifi-psk-file PATH` after `--wifi-assoc` adds the bounded WPA2-PSK checkpoint.
The private file contains the test passphrase; only its derived PMK is embedded
in this experimental EFI. Keep that file and image private and out of source
control. There is no password prompt. A fresh CPU-generated SNonce is required
on every boot. The path exchanges EAPOL M1/M2/M3/M4, verifies the M3 MIC and
AES-wrapped key data against the selected AP's RSN, then waits for positive M4
TX completion before installing the CCMP pairwise and CCMP/TKIP group keys.
Authenticated retries may resend M4 but never reinstall keys. Native crypto
cores come from pinned OpenBSD sources; the firmware key layout follows Linux
v6.12 MLD and its TKIP MIC-key orientation follows wpa_supplicant 2.11.
The final marker is `AX211_WPA2_OK`. It proves only the handshake and firmware
key command replies: the radio is stopped before the shell. Protected data,
RX replay filtering, separate group/rekey handshakes, a persistent interface
and IP/Telnet remain future work. This checkpoint requires a GTK bundled in
M3 and descriptor-v2 PSK/CCMP; unsupported security modes fail explicitly.

`--wifi-data-probe A.B.C.D` after `--wifi-psk-file` extends the checkpoint with
one protected ARP request and a matching CCMP unicast reply. It uses sender IPv4
0.0.0.0 and never adopts the probed address. The temporary target is an explicit
build argument, not a driver default. The firmware handles TX CCMP IV/PN/MIC;
RX validates the selected AP, hardware CCM/MIC status, key0 and increasing PN
per TID, then converts native 802.11/LLC into a bounded Ethernet frame. RX pad
is skipped after the crypto IV, and protocol lengths exclude leftover trailers.
The final marker is `AX211_DATA_OK`, followed by verified radio stop and the
RAM-root shell. This is unicast proof only: group TKIP, live rekeying, reusable
runtime rings and attachment to the running IP stack remain subsequent work.
A missing reply can also mean the target does not answer probes or uses only
broadcast replies; this profile accepts unicast replies only. No IP address is
claimed and no persistent LAN configuration or host routing is changed.

The command/NVM host regression also replays that physical PNVM status. Replace
`wifi-nvm.c` in the test path and output name with `wifi-scan.c` to run the scan
regression, or `wifi-wpa-transport.c` for the full WPA2 checkpoint, using the
same firmware/PNVM arguments. The tests use public synthetic keys only:

```sh
python3 tools/x86_64/wifi_check.py \
  --firmware build/x86_64-network/wifi-reference/iwlwifi-ma-b0-gf-a0-89.ucode \
  --pnvm build/x86_64-network/wifi-reference/iwlwifi-ma-b0-gf-a0.pnvm
# On this Mac's toolchain, add:
# --sysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
```

`--telnet-port` changes the localhost forwarding port. Network app packages
include ifconfig, ping, host/nslookup, dig, telnet/telnetd and a small HTTP
diagnostic, alongside the normal profile. The canonical htget now needs its
TLS integration and entropy support and remains excluded. Existing persistent
images are preserved; rebuilds do not silently install new apps into them.

The driver implements the existing checked `DEVICE_NET_*` contract using
bounded native queues and permanent DMA buffers. It copies octets to/from C16
memory and never submits C16 pointers to hardware. Both TX and RX are polled;
the timer ISR and checkpoint scheduler remain unchanged. QEMU supplies NAT,
while the guest owns ARP, IPv4, ICMP, DNS and TCP. Forwarding binds localhost.
The network acceptance checks an exact binary download, two independent PTYs,
shared files, pipelines, Ctrl-C, reconnects, link loss/restoration, persistence
and orderly shutdown. It writes `network-result.json` beside boot evidence.

## Hosted Linux validation

With Docker running, from the repository root:

```sh
scripts/test-x86_64-linux.sh
```

This selects an actual `linux/amd64` environment, including on an ARM Mac. It
builds RCC and its native runtime with the locked dependencies, executes the
native compiler and continuation regression tests, then builds a small hosted
Riplix installation. Acceptance checks require an x86_64 ELF kernel and exercise
init, the Bourne shell, independently loaded native executables, shared libc,
pipes, fork/wait, file copying and persistence across kernel restarts.

The source mount is read-only. Host tools use the separate Docker volume
`braintease-x86_64-target`; registry downloads use `braintease-x86_64-cargo`.
They do not replace the development Mac's `target/` artifacts. Guest checks
create disposable C16 root images instead of opening the normal Fuse-X disk.

The first build downloads a pinned Rust 1.97.1 Debian image, Linux dependencies
and Rust crates. Later runs reuse build caches. Host-tool release optimization
is reduced to make compilation under CPU emulation practical; RCC still selects
its normal native code-generation optimizations.

Results go to `build/x86_64-linux/`:

- `validation.log`: build and compiler-test results.
- `hosted/fuse-x` and `hosted/apps/`: x86 Linux kernel and native packages.
- `shell.log` and `restart.log`: guest transcripts.
- `result.json`: written only after the hosted acceptance checks pass.

An optional output-directory argument selects another results directory. Docker
build caches remain shared. These are Linux-hosted executables and FXE packages;
they require a Linux host and cannot be loaded by the freestanding kernel.

## Freestanding kernel boot

Prerequisites are Rust/Cargo, Node.js, Python 3, Make, Clang, LLVM LLD, QEMU's
x86 system emulator, and x86 UEFI firmware. A Rust toolchain's bundled LLD is
also supported. Common Homebrew
and Linux OVMF firmware paths are detected automatically. On Ubuntu, the usual
packages are `clang lld qemu-system-x86 ovmf`; on macOS, QEMU includes EDK2
firmware and a Rust installation can supply the linker.

```sh
# Compile and run the headless acceptance check, with a 180-second timeout.
python3 tools/x86_64/boot.py test

# Run the same persistence/error suite on an NVMe namespace (512-byte LBAs).
python3 tools/x86_64/boot.py test --storage nvme

# Also exercise read/modify/write with 4 KiB physical/logical NVMe sectors.
python3 tools/x86_64/boot.py test --storage nvme --nvme-lba 4096

# Dedicated GPT partition: two boots, outside-partition hashes, 13 rejections.
python3 tools/x86_64/boot.py test --storage nvme --nvme-partition
python3 tools/x86_64/boot.py test --storage nvme --nvme-partition --nvme-lba 4096

# Start Riplix and type into its QEMU window.
python3 tools/x86_64/boot.py run

# Use another persistent disk, initialized once if it does not exist.
python3 tools/x86_64/boot.py run --disk build/my-riplix-root.img

# Boot entirely from the embedded RAM root, resetting files at each launch.
python3 tools/x86_64/boot.py run --ram-root

# Boot a GPT/FAT32 EFI partition through emulated USB/xHCI.
python3 tools/x86_64/boot.py test --ram-root --usb-boot

# Build the production USB image, without test exit-port operations.
python3 tools/x86_64/boot.py build --ram-root --usb-boot

# Add a compile-time read-only NVMe/GPT probe, keeping the shell on RAM root.
python3 tools/x86_64/boot.py test --ram-root --nvme-probe --probe-iommu --usb-boot
python3 tools/x86_64/boot.py build --ram-root --nvme-probe --usb-boot

# Check that existing production image without rebuilding anything.
python3 tools/x86_64/check_image.py \
  build/x86_64-boot/normal/usb-boot.img --normal --output build/usb-check

# Retain explicit legacy PIC/PIT regression coverage.
python3 tools/x86_64/boot.py test --stage runtime --interrupt-controller pic

# Exercise production console setup on actual high physical PCI memory.
python3 tools/x86_64/boot.py test --stage runtime --high-framebuffer

# Fine-tune the smooth gterm font's cell height (default: 24 pixels).
python3 tools/x86_64/boot.py run --font-height 22

# Select the original bitmap console instead.
python3 tools/x86_64/boot.py run --font rom

# Build the UEFI loader, ELF kernel and disposable FAT16 boot image only.
python3 tools/x86_64/boot.py build

# Pause at reset, exposing the debugger on localhost:1234.
python3 tools/x86_64/boot.py run --debug

# Run the smaller C16 runtime acceptance program instead of Riplix.
python3 tools/x86_64/boot.py test --stage runtime

# Test context mechanics separately from the filesystem and shell.
python3 tools/x86_64/boot.py test --stage process

# Retain the earlier read-only kernel bootstrap acceptance check.
python3 tools/x86_64/boot.py test --stage bootstrap

# Exercise CPU exception diagnostics, including the double-fault IST stack.
python3 tools/x86_64/boot.py test --stage runtime --fault invalid_opcode
python3 tools/x86_64/boot.py test --stage runtime --fault null_page
python3 tools/x86_64/boot.py test --stage runtime --fault stack_overflow
```

Click the QEMU window, then try these commands at the inherited `fuse-x#`
prompt:

```text
help
ls /bin
hello from-x86_64
echo hello-from-ram > /tmp/note
cat /tmp/note
echo pipeline | cat
sh /demo.sh
appcheck
timercheck
check
pipecheck
shutdown
```

This is the same Bourne shell used by hosted Riplix, including its line editor
and job-control sources. `/bin/sh`, `echo`, `cat`, `ls`, `wc`, `sleep`, `true`,
`false`, `cp` and `test` are independently compiled packages. Copying one to
another executable path works through the same filesystem loader. `appcheck`
and `abicheck` exercise the independent application ABI; `check`, `hello` and
`pipecheck` retain the earlier prelinked fixtures. Use `shutdown` for orderly
process teardown and filesystem sync. Normal launches preserve files in
`build/x86_64-boot/rootfs.img`. The first run seeds that dedicated raw C16 V3
image with the built applications. Subsequent launches and kernel rebuilds
preserve it, including its installed application versions. To try a newly
seeded installation, use a different `--disk` filename. Existing images are
never overwritten by seeding. The launcher accepts regular raw C16 V3 images
and rejects symlinks, device nodes and outer FAT images. A launcher lock plus
QEMU's file locking prevents concurrent launches from writing the same disk.
No physical disk or ordinary Fuse-X root image is opened.

`--ram-root` retains the earlier disposable session: every boot starts with the
embedded seed and writes remain in RAM. The console status identifies which
mode is running. `build` creates the boot image and root seed; it does not
initialize or modify the persistent user disk. For an initial standalone USB
RAM experiment, build with `--ram-root`; the default image expects virtio storage.

The PE/COFF UEFI loader validates and loads an embedded, statically linked PIE
ELF kernel into a suitable free physical range. It applies only checked
`R_X86_64_RELATIVE` relocations; external dependencies, interpreters and symbol
resolution are rejected. It captures the graphics layout, memory map, RTC
snapshot, ACPI 2 RSDP and a calibrated TSC frequency before `ExitBootServices`.
Loader failures use the UEFI text console while firmware services are valid;
kernel failures/exceptions also reach the framebuffer after it initializes.
The PE loader carries an explicit native-data base relocation and verifies it
on entry.

The ELF kernel immediately switches to its own 1MiB native stack, with a
1MiB unmapped guard below it. Larger generated CLI activations need this space:
awk, BASIC and dc exceed the earlier 64KiB bootstrap stack. Arbitrary-frame
stack probing/frame-size metadata still require compiler coordination. It installs its
own GDT, IDT and TSS, including independent double-fault and IRQ stacks, and
replaces firmware page tables with supervisor identity mappings for the first 64 GiB.
Null and stack-guard pages are absent. Kernel instructions and constants are
supervisor-readonly after relocation, with CR0.WP enforced; application pages
still await a full W^X/isolation milestone. The physical-page allocator uses only
conventional memory, leaving loader/kernel pages, firmware tables and device
regions reserved; acceptance checks exercise distinct pages, reuse and zeroing.
All code after handoff runs without firmware services, Linux or macOS calls.

RCC's native backend cross-compiles the existing C16 kernel IR to x86_64 ELF
from the ARM development Mac. `rcc-native-object --triple x86_64-unknown-none`
selects the explicit ISA and exports `rcc_native_entry(ctx)` without a host
`main`/runtime-start wrapper. Hosted defaults remain unchanged. This first
cross-target path is for the C kernel; architecture-dependent RFL library
selection still needs a frontend target configuration.

The freestanding runtime implements checked C16 memory, globals, function
tokens, string/memory operations, allocation and saved continuation contexts.
Riplix reuses the shared kernel, init, Bourne shell, SDK and process-check
sources. Exec opens packages through the ordinary kernel path, including its
permissions, argv/environment validation, descriptor and process setup.
Small prelinked manifests remain available for bootstrap checks.

`rcc-native-object --application MANIFEST.json --triple x86_64-unknown-none`
emits the existing application continuation ABI as an x86_64 ELF object and
records its explicit target identity. `app_link.py` adapts `fuse-x/cc.py --static`
to build RXE1 packages: a bounded binary header, function/helper tables,
initializers and a flattened statically linked ELF PIE. Padding inside the code
blob preserves the ELF page load bias despite variable-sized RXE1 tables; all
native symbols and relative relocation offsets/addends move together. Only relative native
relocations and named, arity-checked runtime helpers are accepted. The runtime
streams filesystem octets into accounted physical pages, validates metadata,
assigns process-independent function tokens and stages the image before exec
changes private state. Rejected packages leave the calling application intact.
The code remains resident across fork and parked contexts and is released when
no process or staged exec owns it. Static guest libc enters kernel syscalls;
there is no host libc, dynamic library resolver or firmware call after boot.
Hosted FXE files and shared-library packages are different containers and are
rejected. RXE1 currently limits each package to 16 MiB.

Saved jump buffers capture guest continuations and frame identities, preserving
memory changes across `longjmp` and surviving fork. Returned frames invalidate
their jump targets. Independently compiled loops and calls reach cooperative
scheduling checkpoints driven by pending hardware ticks. Callbacks never
interrupt the temporary lifetime of an aggregate return value or reenter an
active syscall/checkpoint handler. `rcc_native_poll` remains non-suspending;
interrupts never change C16 memory backing or invalidate a borrowed memory view.

The default interrupt path validates RSDP and XSDT/RSDT checksums and bounds,
then parses MADT local APIC/x2APIC CPUs, I/O APICs and ISA overrides. ACPI spans
must belong to reserved ACPI memory below the initial 64 GiB mapping. FADT
boot-architecture flags and checksummed DMAR/MCFG/IVRS tables are recorded. This is table discovery, not an AML
interpreter, ACPI power management or SCI/device interrupt routing.
The boot CPU uses x2APIC MSRs when supported, otherwise xAPIC MMIO. Other CPUs
are not started. All I/O APIC lines, LINT/NMI inputs and unused LVT sources are
masked; legacy PICs stay masked in APIC mode. Keyboard/UART input is polled.
The local timer is calibrated against the loader's TSC and runs at about 100 Hz,
without using the HP's problematic HPET. Timer and spurious gates use IST2,
protecting the interrupted native stack and its SysV red zone. Entry saves all
general registers and x87/SSE state, clears DF for C, records a native tick,
acknowledges the controller and returns through `iretq`. APIC spurious vector
255 has no EOI. `--interrupt-controller pic` retains the earlier PIC/PIT path,
including its distinct spurious IRQ7/15 rules. No allocation, guest callback or process switching
runs inside the interrupt. A changed tick counter requests work at existing
safe continuation checkpoints; several delayed ticks can coalesce. The TSC
still supplies elapsed time, so scheduler clock accounting does not depend on
counting every timer edge. Idle waits use a deadline check and `sti; hlt` to avoid
losing a wakeup. Both controller paths pass Q35/OVMF tests. The same RAM-root
USB image passes remote KVM with the HP CPU and x2APIC; its actual physical
interrupt controllers now also support the photographed physical shell and
passing timer/process checks. Repeated physical boots remain to be tested.

The kernel owns scheduling and its 64 process slots. Runtime snapshots preserve
private globals, stacks, heap metadata and live continuation values; sparse
RAM pages are reference-counted and copied when a context saves changed data.
A flat active view avoids adding a page lookup to every generated load/store.
Switching restores only pages whose snapshot differs. The kernel arena stays
shared. Exec resets private initializers, allocator and stack while preserving
the arguments prepared by the kernel. Discarded contexts release physical
pages; the driver verifies that all context pages are reclaimed at shutdown.
Metadata allocations use the same accounted physical-page allocator as RAM.

The default persistent disk stream uses a modern virtio 1.x PCI block driver. Firmware
assigns PCI resources; segment-zero configuration-space discovery identifies
the endpoint and validates its memory BARs and capability chain. Checked
supervisor device mappings handle BARs above the initial 64 GiB RAM map using
the CPU's physical-address limit and an uncached PAT entry. New page tables
come from the accounted physical allocator; this does not expand C16 memory.
The driver negotiates VERSION_1/FLUSH, uses an eight-entry split queue and one
outstanding request, and checks completion identity, length and status. Native
DMA buffers remain separate from C16 cells. Transfers use 512-byte sectors;
partial sectors use read/modify/write. Native file/cell adapters retain the
filesystem's 2,048-octet blocks, little-endian full 16-bit cells, checked spans
and private dirty-page accounting. A five-second TSC deadline bounds each I/O;
after timeout its DMA buffers stay resident and are never reused. Device errors
return short I/O/failure; unsupported setup fails explicitly. Flush requests
reach the backend on filesystem flush/close/sync and final orderly shutdown.
The first adapter is synchronous and polled; it does not schedule processes
inside a native I/O helper, and interrupts remain timer-only.

`--storage nvme` selects a polled NVMe 1.x NVM-command-set driver instead.
It discovers PCI class 01:08:02, validates the register BAR/doorbell span,
resets/enables the controller, identifies one active namespace and creates
an eight-entry I/O submission/completion queue. Admin and data buffers are
page-aligned native RAM; each transfer fits a single 4 KiB PRP page. Queue
phase, command ID, submission queue/head, completion status and TSC deadlines
are checked. Read/modify/write supports 512-byte through 4 KiB LBAs, with no
metadata/protection format. Flush reaches the device; a timeout leaves DMA
buffers resident and forbids reuse. MSI/MSI-X/INTx are disabled for polling.
The full shell, two-boot persistence and five error cases pass for both 512-byte
and 4 KiB QEMU namespaces. Tests use regular disposable image files only.

The HP controller was inspected
through Linux's read-only Identify command: WD SN560, NVMe 1.4, one namespace,
512-byte LBAs, no metadata/protection, SQES=0x66, CQES=0x44. This confirms its
format fits the implemented subset. The later physical USB boot independently
confirmed native Identify/reads and GPT validation, then dedicated-partition
file/executable writes and recovery after a cold boot. Our native driver never
resets the physical controller while Ubuntu owns it.
`--nvme-probe --ram-root` now builds the physical read-only probe. Native
64-bit disk addressing bypasses the raw C16 FILE size limit for this probe.
A command allowlist at submission accepts only Identify, queue setup and NVM
Read: namespace Write, Flush, Format and other admin commands are refused before
a doorbell can be rung. Test builds directly exercise those refusals. No normal
storage selfcheck writes are compiled into a probe build. The shell still uses
the embedded RAM root; it cannot open NVMe as its filesystem stream.

Before changing NVMe bus mastering, the driver checks DMAR DRHD/SATC/SIDP/RMRR
records and MCFG windows. Every advertised Intel VT-d unit must have translation
and implemented protected-memory regions disabled. It rejects an unhandled AMD
IOMMU, kernel DMA buffers overlapping reserved RMRRs, malformed PCI ECAM/extended
capabilities and ATS/PRI/PASID endpoints. It never disables or rewrites an IOMMU.
An unsupported DMA handoff reports a skipped probe and continues with RAM root.
PCIe No Snoop and Relaxed Ordering are cleared for coherent, ordered native DMA;
bus mastering stays off through BAR inspection/reset. Device interrupts stay
masked. After reads complete, the controller is disabled and bus mastering is
cleared before the RAM-root shell starts. A fatal controller error still stops
explicitly, with its DMA buffers resident and on-screen diagnostics.

The bounded streaming GPT parser requires the protective MBR, both headers and
both arrays to pass CRC/bounds checks and agree. It rejects overlapping or
out-of-range partitions, duplicate GUIDs and unsupported entry sizes/counts;
it does not repair GPT, select a root or mount any partition. The fixed console
header retains the verified/skipped/error result, and boot output lists the
model, capacity, sector size and partition LBAs. The user's HP photograph now
confirms these expected results on the physical SSD.
Host tests use actual HP firmware/GPT captures. QEMU checks cover 512/4096-byte
sectors, a sparse 4 TiB namespace with LBAs above 32 bits, the exact HP GPT
metadata, a missing disk and a damaged primary header. Virtual backends are
read-only. Small images are fully hashed before/after; multi-terabyte sparse
fixtures explicitly record only first/last-MiB fingerprint coverage.

The writable raw-root adapter still rejects namespaces at or above 2 GiB.
`--nvme-partition --storage nvme` instead requires an exact GPT disk GUID,
partition GUID, first/last LBA and sector size. The selected entry must have
project-private type `a90c10b4-cb9d-4a61-8b6a-e8f4d16938cf`, zero attributes,
native partition bounds and a matching C16 V3 signature/superblock geometry.
Large reservations (including 30 GiB) are supported; only the filesystem's
declared byte length must fit the signed C16 stream limit, smaller than 2 GiB.
Unused reserved space stays outside the exposed root window.
No selection by label/index, automatic formatting or GPT repair occurs.
Discovery is read-only. Only after it succeeds does the adapter expose the
filesystem's bounded byte window; every write is checked again at the final
NVMe command boundary, and flush is refused before selection. Test scratch
writes run AFTER window selection and are confined to that window.

Tests create their own small GPT disk with EFI/Ubuntu canary partitions and a
separate Riplix seed. Both boots must preserve a full hash of EVERY byte outside
the Riplix partition, including both GPT copies and immediately adjacent sectors.
Eight additional rejection boots cover wrong disk/partition/type, attributes,
moved geometry, filesystem format/size and GPT corruption. Failed boots leave
their entire disposable disks byte-identical to the pre-boot state.
The damaged-primary-GPT test freezes its backend read-only: UEFI was observed
repairing that header from the valid backup before our loader ran on a writable
fixture. This firmware behaviour is recorded in `firmware-gpt-repair.json` and
is distinct from the native parser's strict refusal to repair or accept it.

Build/run requires `--root-identity identity.json`; run also requires an EXISTING
regular GPT `--disk` image. These modes never create/seed a GPT image or accept a
physical device/symlink. The explicit version-1 identity schema is:

```json
{
  "version": 1,
  "disk_guid": "12345678-1234-5678-9abc-123456789abc",
  "partition_guid": "5946f329-8d99-4333-b387-04ea4fb7094b",
  "first_lba": 8192,
  "last_lba": 40959,
  "sector_bytes": 512
}
```

Those are DISPOSABLE FIXTURE identities, not HP install coordinates. For example,
after the 512-byte-sector test, its `test-root-identity.json` and
`test/test-root.img` can be used with run and the same output directory.
The HP now has the approved dedicated 30GiB p3 and accepted native persistence.
Internal EFI installation and boot-entry changes remain separate work. Consult
the current payload/fallback record before selecting a physical boot file.
The [dedicated storage record](hardware/hp-nvme-persistence.txt) retains the
allocation, backups, identities and physical acceptance history.

The minimal seed is 16MiB C16 V3 (4MiB for the earlier RAM bootstrap);
the offline collection uses 128MiB. The adapter
requires a writable, FLUSH-capable window smaller than 2 GiB: either the entire
raw image, or the selected partition's declared filesystem length. The GPT
namespace itself may be much larger. Hotplug/resize support is pending.
Use `shutdown` before stopping QEMU; crash consistency/journaling is separate
work. The RAM mode copies the embedded seed into guest RAM instead.
Clock/device requests
use a small platform adapter backed by the calibrated TSC and the pre-handoff RTC
snapshot. Console output goes to the PC UART and a native RAM shadow buffer. Scrolling
copies RAM rather than reading uncached video memory. Dirty screen regions are
presented in batches (at most about 60 Hz during continuous output), with an
explicit flush before input/idle, status updates and fatal diagnostics. A prompt
without a newline is visible before input waits. The timer ISR never renders.
The framebuffer uses 4 KiB write-combining PAT mappings; device registers remain
uncached and adjacent pages keep their own policy. MTRRs are preserved, so a
requested WC mapping is not a guarantee of the effective hardware type.
`--framebuffer-cache uc` retains an uncached comparison/fallback profile.
The backbuffer is a permanent platform allocation outside C16 process memory;
at 1920x1200 it needs about 8.8 MiB. Host pixel tests cover poisoned video RAM,
RGB/BGR, padded stride, edge guards, UTF-8/ANSI and batching. Boot tests compare
identical eight-scroll workloads and verify every resulting pixel. Measured
samples: about 2.5x faster in TCG and 4.1x in an HP KVM guest, using the same
mapping policy for both paths. These are VM measurements, not physical-panel
performance claims. The production image passed full USB shell acceptance in
both environments; the user subsequently confirmed that scrolling is super fast
on the actual HP and that the shell scripts/checks passed.
On macOS the default font reuses native gterm's renderer and font selection:
the default iTerm2 profile, `FUSE_X_TERM_FONT`, or Menlo 13 as fallback.
The `rcc-terminal-font` host tool exports grayscale glyph masks into a compact
atlas during the build. The kernel blends those embedded pixels directly;
it needs neither CoreText nor a host font service after boot. Font data stays
in the generated build artifacts. The console includes ASCII and the banner's
box/block characters; this is not gterm's full Unicode/shaping support.

The default atlas has 24-pixel-high cells, between the original 8x16 font and
its previous doubled size. `--font-height 12` through `48` adjusts it without
enlarging individual bitmap pixels. Both the header and console use its metrics.
Long lines wrap and the console scrolls while keeping the status visible.
Basic ANSI cursor movement and erase controls support the shell's line editor;
styling retains the bootstrap palette.
Large displays can use an additional integer scale automatically;
`--font-scale 1` through `8` overrides that choice, limited to fit the display,
and `0` restores automatic scaling. Serial retains the full transcript.

On other build hosts, automatic font selection uses the existing
[Nano-X ROM font](../../ports/graphics/fonts/LICENSE); `--font rom` explicitly
selects it on any host. To reuse a Mac-exported atlas on Linux, supply
`--font-atlas /path/to/font.atlas` (generated under `normal/` or `test/`).
`--font gterm` explicitly requests the macOS exporter. The resulting boot image
is self-contained regardless of which machine generated its font. The exporter
requires access to macOS's font service and checks for fixed-width metrics;
sandboxed builders can use a previously exported atlas instead.

Successful Riplix acceptance waits for `/init` and the shell, then sends actual
QEMU key events through the i8042 keyboard and kernel TTY line discipline.
It checks Shift, punctuation and Backspace, the shell's PID, directory reads,
exec argument passing, a fresh test root, file creation/readback, and sleep.
It also runs `cd /bin` followed by bare `ls`, and verifies that a bank-spanning
SDK heap allocation stays inside the bounded arena.
The shared `check` program verifies private globals/stack, shared open-file
offsets, concurrent children, wait status, timer wakeups, failed-exec rollback
and 20 exec/slot-reuse cycles. `pipecheck` transfers 20,000 cells across a
blocking, wrapping ring and verifies EOF and child status. Acceptance captures
the framebuffer, types `shutdown`, and requires clean teardown and sync.
Independent checks cover function-pointer/data initializers, bank-spanning
globals and heap, inherited jump targets, fork isolation, exec reset, wait
status and failed-exec recovery. Shell checks include scripts, loops, pipelines,
redirection, substitution, exit status and executing a copied package. Shutdown
requires every loaded image and runtime physical page to be reclaimed.

Default storage acceptance creates its own disposable `test/test-root.img`,
runs the shell suite, writes a persistent marker, copies an executable and
runs `diskcheck`. A fresh QEMU process with the same disk must recover all
three. `diskcheck` verifies 20,480 full cells, including high bytes, partial
writes across block boundaries, indirect blocks and EOF. Test-only native
checks restore their scratch disk bytes before mounting and verify partial
sector/buffer transfers, bounds, read-only rejection and flush completion.
Separate QEMU boots reject a missing/read-only disk and inject read, write and
flush failures through QEMU blkdebug; their individual transcripts stay under
`storage-errors/`. None of these tests opens or resets `rootfs.img`.

The production root-stream adapter also has a host harness with injected I/O
and flush failures, bank/page-edge transfers, dirty tracking and cell/octet
representation checks:

```sh
cc -O1 -Wall -Wextra -Werror -fsanitize=undefined \
  tools/x86_64/tests/root.c -o /tmp/riplix-root-test
/tmp/riplix-root-test
python3 tools/x86_64/tests/test_storage_launcher.py
```

Boot tests additionally exercise repeated asynchronous timer delivery and HLT
wakeup with all GPRs, XMM0..15, x87, IF/DF and red-zone sentinels live. The test
ISR deliberately destroys SIMD/x87 state before restoring it. Software-injected
spurious vectors test their no-in-service paths. `timercheck` runs a syscall-free
busy parent while a child sleeps, wakes and signals it; the parent then reads
the child's pipe and verifies its wait status. This covers deferred scheduling
and signal delivery during a busy app, beyond merely printing a boot marker.

The host-side `tests/images.c` harness exercises the production loader without
executing x86 code. It rejects malformed metadata and non-octet input, injects
allocation failure, checks fork ownership and verifies page reclamation. For
example, on a host with a working C SDK:

```sh
cc -O1 -ffreestanding -fno-builtin -fsanitize=undefined \
  tools/x86_64/tests/images.c -o /tmp/riplix-images-test
/tmp/riplix-images-test build/x86_64-boot/userspace-bounded/apps/appcheck.fxe
```

These are trusted programs using the private native ABI, all running at ring 0.
Context selection occurs at safe checkpoints rather than in the IRQ handler.
A busy loop in a prelinked program without checkpoints cannot yet be preempted;
nor can an arbitrary long native helper. CPU exceptions still use the kernel's
IDT. Per-process hardware isolation, W^X, arbitrary-instruction preemption,
ACPI device routing/power management and reclaiming firmware memory,
shared guest libraries and general graphical devices remain later work.
Physical memory exhaustion is bounded by the page allocator; comprehensive
per-process budgets and recovery from every out-of-memory case remain work.
The keyboard driver supports basic US scan-set-1 input; USB/HID and other
layouts require drivers. COM1 can also supply input independently of PS/2.
The two standard 32-bit RGB/BGR framebuffer layouts are supported; other
formats fail explicitly. GOP addresses above 64 GiB use checked supervisor
device mappings, independently of the bounded RAM allocator/C16 pointer model.
Only the visible stride/height span is requested, with CPU physical/canonical
address bounds and page-granular framebuffer cache policy. RAM-backed scrolling
and write combining are implemented as described above. `--high-framebuffer` is a test-only profile that runs
production console initialization/readback on a backed PCI test BAR at 768 GiB,
then restores the actual GOP display. It tests high physical addressing, not
Intel GPU firmware or display modesetting. The host ACPI parser harness also
accepts the actual HP MADT fixture and rejects malformed checksums/topology:

```sh
cc -O1 -Wall -Wextra -Werror -fsanitize=undefined -DACPI_HOST_TEST \
  tools/x86_64/tests/acpi.c tools/x86_64/boot/acpi.c -o /tmp/riplix-acpi-test
/tmp/riplix-acpi-test tools/x86_64/tests/fixtures/hp-envy-madt.bin
```

`--stage bootstrap` retains the earlier read-only acceptance at the `/init`
loading boundary. `--stage process` runs the same native-ABI fixture that can
also run as a hosted executable: it tests slot 63, invalid/occupied slots,
private bank-spanning heap data, shared memory, switch/drop, slot reuse, exec
initializers and physical-page reclamation without depending on kernel syscalls.

Unimplemented native services bind to named fatal traps, listed in each
build's `unsupported-services.json`; they never silently succeed or resolve
to host libc. The runtime test produces eight `Y` checks for C16 layout,
initializers/callbacks, wide integers, allocation, strings and hardware floats.
The same fixture is registered in `rct` as `test_freestanding_native`.

QEMU uses one CPU, a Q35 machine, TCG emulation, 512 MiB RAM, no NIC, a copied
firmware variables file and a snapshot of the generated boot disk. A separate
virtio device attaches the persistent raw root without a snapshot. It never
attaches a physical disk. Firmware loading the image from virtual SATA does
not establish a Riplix SATA or USB driver.
`--usb-boot` instead creates a 260 MiB GPT image with a 256 MiB FAT32 EFI System
partition, and attaches it through virtual xHCI/USB storage. Its removable-media
path is `EFI/BOOT/BOOTX64.EFI`. This generator writes regular image files only.
`--accel kvm` and `--cpu host` support Linux testing; `--memory` adjusts VM RAM.
512 MiB leaves more firmware/loading headroom; the HP has 16 GB. Newer Ubuntu
OVMF intermittently reported `Not Found` before entering our loader at both
256 and 512 MiB. Retrying the byte-identical final image passed the full suite.
Its cause remains unresolved; neither increased RAM nor the added relocation
is a proven complete fix. A VM uses emulated display/USB/interrupt
devices, so even running on its CPU does not certify the physical devices.
`check_image.py` runs the shell suite against an existing RAM-root image,
including production builds, without requiring Rust/Clang or rebuilding it.

Results go to `build/x86_64-boot/`. Normal images are under `normal/`; the test
image under `test/` includes a QEMU-only exit-port operation. Each contains
`kernel.elf`, `esp/EFI/BOOT/BOOTX64.EFI` and `boot.img`. Fault builds use their
own `test-FAULT/` subdirectory. Shared kernel IR is cached in `riplix-ir-bounded/`.
Independent packages and cached SDK IR live in `userspace-bounded/`; the disposable
outer FAT16 boot disk is 64 MiB. Storage acceptance also records
`storage-first-boot.log` and `storage-second-boot.log`; `result.json` records
both boots and the storage rejection checks after they pass. `--ram-root`
runs the earlier single-boot suite without attaching a root disk.
Each image's `build.json` records its heap profile; successful `result.json`
records the same selection.
`serial.log`
records the boot stages, `qemu.log` records emulator diagnostics, and
`result.json` is written only after every acceptance marker and QEMU's expected
exit status are verified. Riplix's keyboard test also writes `screen.png` just
before shutdown. QEMU status 33 denotes normal acceptance; deliberate
fault tests require status 35 and the correct exception vector. A timeout,
unexpected exception or triple fault fails. The Python wrapper translates a
verified result to success. Use a separate `--output` directory for concurrent
tests, and build the normal image for any later hardware experiment.

Use `--output DIR` to select another build directory. To supply matching
firmware files explicitly:

```sh
python3 tools/x86_64/boot.py test \
  --firmware-code /path/to/OVMF_CODE.fd \
  --firmware-vars /path/to/OVMF_VARS.fd
```

`OVMF_CODE` and `OVMF_VARS` provide the same overrides. The variables template
is copied for every run; the installed template is never modified.

## Next milestones

The [Riplix x64 master plan](../../docs/x64-master-plan.md) is the main delivery
roadmap, including parallel ownership and acceptance gates. Keyboard/terminal
work and a concrete process integration contract are the first active slices.
The first console candidate passes host checks and exact two-boot QEMU
acceptance: Escape/navigation/function-key octets, real terminal geometry,
bounded ANSI/colors/line drawing, persistence and firmware reset. HP input
acceptance and installation remain pending; details are in the master plan.

The [2026-10-21 roadmap reconciliation](hardware/hp-integration-plan.txt) maps
the user's GUI/laptop wishlist to current code, dependencies and proposed
parallel scopes. Its current-state section supersedes older handoff snapshots.

1. Finish physical GTK-rotation acceptance and the reliable remote development
   loop. NVMe persistence, apps, Wi-Fi/IP, services, SSH/SFTP/pkg and orderly
   reboot have already worked on HP. The latest candidate passed a ten-minute
   held SSH session. A later periodic check observed a real GTK refresh without
   reassociation; held-session continuity across rotation remains open. Profiles/DHCP and
   user-facing network selection remain integration work.
2. Fix physical keyboard and terminal geometry, then bring mux, Commander/mc,
   Rift and other supported applications through the existing package catalog.
   mux does not require graphics. Audio and crypto-dependent applications need
   their respective bare-metal services, not merely an executable rebuild.
3. In parallel, follow the accepted [execution priority](../../docs/arch/native-x64.md):
   measure switching, integrate reviewed hardware process backing under current
   checkpoints, separate privileged helpers, then ring 3 and true timer-driven
   userspace preemption. This no longer waits for all heavier applications.
   The process-memory subtree remains an isolated prototype until integration
   is explicitly reviewed; LP64 is not required for C16 isolation.
4. Add an independently tested internal EFI boot alongside Ubuntu and retained
   USB recovery, then verified candidate updates and failed-boot recovery.
   Internal boot need not wait for GUI, LP64 or a replacement filesystem.
5. Develop pointer transports/input, a software-rendered HP GX desktop, PCM
   audio and laptop power support in bounded coordinated scopes. Reuse the
   shared GUI/LVGL/terminal contracts; hosted backends are not HP drivers.
   Bluetooth follows USB/HCI and audio support. GPU acceleration is a later
   backend behind stable surface/presentation contracts.
6. Design native LP64/static ELF64 SDK contracts and safer storage in parallel;
   stage each behind mixed-ABI or disposable-media acceptance. Threads/SMP,
   native kernel migration and advanced GPU work have separate ownership and
   acceptance gates. Follow the guest toolchain owner's
   [bootstrap boundary](../../rcc-portable/native-sdk/OS_BOOTSTRAP.md) rather
   than counting host platform objects or hosted FXE as freestanding userspace.

The current UEFI image embeds the kernel; the ordinary HP profile uses the
persistent NVMe root, while recovery profiles can embed a RAM root. Separate
EFI payload loading/update tooling is still a later boot-format change.

Interface references: [UEFI system table](https://uefi.org/specs/UEFI/2.11/04_EFI_System_Table.html),
[boot services](https://uefi.org/specs/UEFI/2.11/07_Services_Boot_Services.html),
[graphics output protocol](https://uefi.org/specs/UEFI/2.11/12_Protocols_Console_Support.html),
[Intel system programming manuals](https://www.intel.com/content/www/us/en/developer/articles/technical/intel-sdm.html),
[ACPI 6.5 table/MADT definitions](https://uefi.org/specs/ACPI/6.5/05_ACPI_Software_Programming_Model.html),
[Intel legacy PIC/PIT register reference, sections 5.6/5.7 and 9.2/9.3](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/82801db-io-controller-hub-4-datasheet.pdf),
[Virtio 1.2 PCI and block interfaces](https://docs.oasis-open.org/virtio/virtio/v1.2/virtio-v1.2.html),
[Intel VT-d interface](https://cdrdv2-public.intel.com/831418/vt-directed-io-spec.pdf),
[UEFI GPT format](https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html),
[NVMe 1.4c controller and NVM command interface](https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_4c-2021.06.28-Ratified.pdf),
[QEMU block error injection](https://www.qemu.org/docs/master/devel/testing/blkdebug.html),
and [QEMU system emulation](https://www.qemu.org/docs/master/system/introduction.html).

Read-only parser/handoff host checks (use a working host C SDK):

```sh
python3 tools/x86_64/tests/test_gpt.py
python3 tools/x86_64/tests/test_root_window.py
cc -O1 -Wall -Wextra -Werror -fsanitize=undefined \
  tools/x86_64/tests/root-window.c tools/x86_64/boot/root-window.c \
  tools/x86_64/boot/gpt.c -o /tmp/riplix-root-window-test
/tmp/riplix-root-window-test build/x86_64-partition/test/test-root.img 512
cc -O1 -Wall -Wextra -Werror -fsanitize=undefined \
  tools/x86_64/tests/dma.c -o /tmp/riplix-dma-test
/tmp/riplix-dma-test tools/x86_64/tests/fixtures/hp-envy-dmar.bin \
  tools/x86_64/tests/fixtures/hp-envy-mcfg.bin
```

For focused native probe regressions, use `--stage runtime --ram-root
--nvme-probe`. `--nvme-lba 4096` changes the virtual namespace format;
`--probe-disk IMAGE` attaches an existing regular image read-only, and
`--probe-expect gpt-error`/`missing` select test-only rejection expectations.
No launcher accepts a device node or symlink as a probe disk. `gpt_image.py`
creates new sparse fixtures with exclusive creation; it never replaces a disk.
`check_image.py --probe-disk IMAGE --probe-iommu` adds the same read-only virtual
devices when accepting an already-built production USB image.

To accept an already-built production partition-root USB with disposable
fixture identities, use `check_image.py --normal --partition-root-seed ROOT
--root-identity JSON`. It creates a NEW private GPT fixture, runs two boots,
checks exact persistence and full outside-partition hashes, then removes the
fixture. It never attaches an existing writable root disk. For example:

```sh
python3 tools/x86_64/check_image.py \
  build/x86_64-partition-production/normal/usb-boot.img --normal \
  --partition-root-seed build/x86_64-partition-production/normal/root.img \
  --root-identity build/x86_64-partition/test-root-identity.json \
  --output build/x86_64-partition-production/production-check
```


## Unix scheduling utilities

The `offline` and `system` application profiles include `tree`, `getopt`,
`cron`/`crond`, `at`, `atrun` and `cal`. The `minimal` driver/bootstrap profile
is unchanged. These are freestanding RXE1 x86_64 programs using the bounded
C16 compatibility ABI, not the macOS Fuse-X executables.

The root producer seeds `/etc/crontab`, `/etc/services/cron.ini` and a private
`/var/spool/at` queue whenever cron is selected. `crond` is a symbolic link to
`cron`. Existing configuration is preserved. Scheduling is disabled by default;
a system using svcd can explicitly add `cron` to its enabled services, preserving
network/SSH entries already present. No scheduler is added to kernel or driver
test startup. See [the userspace guide](../../ports/fuzix/README.md#tree-option-parsing-and-scheduled-jobs).

A focused acceptance fixture reuses the ordinary application builder and an
already-built virtio-root boot image, without rebuilding the kernel or updating
a hardware image:

```sh
python3 tools/x86_64/unix_check.py build
python3 ports/fuzix/tools/test_unix_batch.py --target x64
```

`--x64-boot DIR` selects a different existing virtio-root boot build. The default
is `build/x86_64-boot/normal`. QEMU receives a private root image, copied firmware
variables, a boot-image snapshot and no network or physical disks. The shared
behavior matrix includes shell quoting, concurrent append/queue workers,
calendar and timezone checks, and manual svcd start/stop of cron. The transcript
is saved to `build/unix-tools/x64-tests.log`.
