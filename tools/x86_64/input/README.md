# Native HP touchpad slice

`boot/i2c-designware.c` implements a bounded polling master; `boot/hid_i2c.c`
implements HID-over-I2C power/reset, descriptors, feature mode and pointer reports.
`boot/i2c-hp.c` binds the exact captured HP profile. It uses no DMA, IRQ routing,
GPIO configuration writes, dynamic allocation or guest pointers.

Run `python3 tools/x86_64/input/check.py` for the focused host fixtures. They
cover FIFO-sized and larger repeated-start transfers, abort/timeout paths,
platform guards, actual HP descriptor parsing and mode-setting wire format.
They are not physical touchpad acceptance. The integration owner performs one
combined hardware trial. `capture-linux.py` is a read-only discovery helper.

## Integration

The caller verifies board8C66 and PCI00:15.2 (8086:7e7a, subsystem103c:8c66), puts
PCI power in D0, enables memory decoding and maps the exact current BAR0 address
for `I2C_HP_CONTROLLER_BYTES` (0x300 bytes). BAR0 need not be page aligned: the
page mapper rounds internally while preserving the returned address offset. The
highest register accessed is LPSS CAPS at0x2fc (32bits). Do not hardcode BAR0 or
round its decoded address down; Ubuntu previously reported0x48192ce000 with a
4KiB resource, which does not establish firmware allocation alignment. Provide
native read32/write32/delay callbacks and static `I2cHp` storage, then call
`i2c_hp_init` with the assigned physical BAR as its fourth argument and poll at
50–100Hz. GPIO community5 at `0xe0d50000` (64KiB) is optional: pass `NULL` if it
cannot be mapped. Its dynamic PADBAR and pin mode must permit raw input reads;
otherwise the driver uses unconditional I2C polling, as OpenBSD permits.

The pointer callback contains relative mouse deltas or absolute touchpad
coordinates, logical ranges, contact identity and buttons. A touchpad adapter
should turn successive positions of the same contact into cursor deltas, retain
cursor position when contact lifts, and always apply button changes. A contact
identity change starts a fresh motion baseline. Only the first active finger is
used in this slice; gestures, scrolling from multiple fingers and palm handling
remain later work. Relative mouse wheel reports are decoded where present.

Malformed, empty and reset reports release contact state and are dropped.
Transient transport errors back off; three consecutive failed reads request
platform recovery. Native glue releases buttons and retries initialization at
1/2/4 seconds, stopping after three attempts unless a valid input report proves
recovery. Unsupported/missing hardware must not prevent boot. GPIO pending
checks read only PADCFG0 raw RX bit1, matching
Linux `intel_gpio_get`. The active-low ACPI polarity is applied after that read.
No touchpad IRQ is unmasked or acknowledged by this driver.

## Firmware BAR assignment

The HP native diagnostics read BAR0 low/high as `0x00000004/0x00000000`
both before and after the PCI D0 transition: a 64-bit memory BAR with no assigned
address. The failure was an unassigned PCI resource; the diagnostic's earlier
"BAR alignment" wording also covered zero and did not establish misalignment.
Ubuntu's later assigned address reflects PCI resource assignment, not a different
DesignWare alignment requirement. Linux allocates within root-bus resource
windows in [setup-res.c](https://github.com/torvalds/linux/blob/v6.12/drivers/pci/setup-res.c#L263),
while OpenBSD reserves existing BAR and bridge spans in
[pci.c](https://github.com/openbsd/src/blob/master/sys/dev/pci/pci.c).

The loader now uses `pci-uefi.c` to copy evaluated root windows and occupied PCI
resources into bounded `BootInfo.pci` arrays before ExitBootServices; firmware
pointers do not survive handoff. Native `pci-assign.c` requires a complete capture
for a new assignment, adds firmware-memory and framebuffer exclusions, sizes
the target BAR with
decoding disabled, and asks the pure `pci-resource.c` allocator for an aligned
hole in an eligible root aperture. It programs and verifies the selected BAR,
restoring the original configuration on failure. Existing devices are not moved,
and the observed Ubuntu address is not used as an allocation hint. Physical BAR
assignment and controller/HID initialization passed on HP with candidate
`4444d9166e54be07dfa2e0ff54512f52375f42018e3857917db5aca17ccd171c`:
`Touchpad ready; reports 0; malformed 0; I2C abort 0`. The user subsequently
confirmed physical movement and calculator button clicks on the same candidate.

The full native resource dump exposed another overly conservative fallback:
an unknown GPU BAR was bounded as 256GiB and an audio BAR as 16MiB, covering
the cached touchpad resource. The actual Linux sizes are 256MiB and 2MiB.
Some firmware descriptors also use inconsistent BAR indices or malformed
extents. Such metadata cannot establish an exact reservation.

`pci-refine.c` now measures unresolved assigned root-endpoint BARs during early
discovery, before interrupts and native drivers start, following Linux
`pci_read_bases` / `pci_size` and OpenBSD `obsd_pci_mem_find`. It disables decoding,
reads size masks and restores the original BARs and command before any logging;
devices are not relocated. Exact native measurements replace those owners'
cached BAR aliases and conservative estimates. Positive bridge windows and
unassigned devices' valid cached resources remain reserved. Host bridges are
not probed as ordinary endpoints. Failure retains the conservative inventory
and prevents allocation; recovery never repeats discovery on active drivers.
The read-only resource telemetry uses `touchpad_status` fields12–14 for snapshot
sizes and bounded word namespaces0x10000/0x30000 for PCI/EFI metadata. It exposes
resource descriptions only, never arbitrary memory contents.

## Captured resources

Read-only Ubuntu evidence: `build/x86_64-graphics-wave/physical/touchpad-linux.json`;
DSDT disassembly: `build/x86_64-touchpad/hp-dsdt.dsl`. The checked descriptor fixture
is `tests/fixtures/hid-i2c-syna32f6.bin` (679bytes). It advertises:

* Mouse input ID2: two buttons and signed relative X/Y.
* Touchpad input ID3: five finger slots, X0..1572, Y0..984, one click button.
* InputMode feature ID4: value3 selects precision touchpad mode.
* Feature ID6 enables SurfaceSwitch and ButtonSwitch. Optional GET_REPORT
  preserves unknown writable fields; a failed GET does not block a feature
  whose writable fields are all known.

The DSDT's GNVS region is `0x65e13018`, with firmware-selected fields at offsets
`0x40c..0x411`: `05 00 2c 20 00 01`. They mean type5, GPIO interrupt mode,
I2C address0x2c, HID descriptor register0x20 and400kHz. Encoded GPDI at offset0x479
is0x00141000. DSDT GNUM maps it to GPIO352; Linux maps that to pin204/GPP_B_0 in
community5. `/proc/iomem` and non-destructive reads confirm community base0xe0d50000,
PADBAR0x600, and PADCFG0 at0xe0d50600 (idle0x80800102). Only raw input state is read.
The device has no separate `_PS0` or power resources in TPD0; controller `_PS0` is
empty. Firmware GPIO setup is retained, and platform discovery owns PCI D0.

Linux's8086:7e7a entry uses133MHz `bxt_i2c_info`, with SDA falling171ns,
SCL falling208ns and hold42ns. Following the full LPSS reset, the driver uses
Linux's formulas (HCNT100, LCNT200), TX hold6 and RX hold1.
Current Linux runtime-suspended BAR timing registers could not be read, so no
claim is made that those fallback counts were captured from this laptop.

The loader snapshot distinguishes descriptor sources: RootBridgeIO.Configuration()
uses `AddrRangeMax` as the inclusive end, while the inspected
[EDK2 PciIoGetBarAttributes implementation](https://github.com/tianocore/edk2/blob/master/MdeModulePkg/Bus/Pci/PciBusDxe/PciIo.c)
writes the BAR alignment mask there (other firmware may leave it zero). BAR
reservations therefore use checked `AddrRangeMin + AddrLen`, with overflow and
address-width checks; root aperture ends remain strictly validated. Failed
metadata is copied into BootInfo's bounded reason field with ROOT/BAR source,
resource type and raw Min/Max/Len. The fake-protocol regression is reproducible
with `python3 tools/x86_64/tests/pci-uefi-check.py`; it does not access PCI hardware.

## Sources inspected before implementation

### Complete comparison and batched correction, 2026-10-22

The repeated native failures prompted a full pass through the upstream PCI,
LPSS, DesignWare, GPIO and HID paths before another hardware update. Detailed
function references and source hashes are in
`build/x86_64-touchpad/source-audit.json` and
`build/x86_64-pci-uefi/audit.json`; focused receipts remain separate from physical
acceptance.

The resulting batch corrects these assumptions together:

* Optional UEFI BAR metadata can be absent or fail despite valid live PCI
  configuration. Use conservative occupied bounds in that case; never free
  failed-call output buffers. EDK2 BAR descriptor Max encodes alignment, while
  root descriptors describe the actual extent.
  Failed queries on stale handles are optional too: mandatory direct root
  configuration reads independently account for assigned BARs and forwarding
  bridge windows. A subtractive bridge needs a downstream inventory and is
  explicitly unsupported by this bounded allocator.
* Existing and new BARs are sized with decoding disabled. Restore a BAR lost
  across D0, verify command/readback, and permit eligible apertures below 4GiB.
  Invalid cached assignments fall back to fresh allocation. Fresh allocations
  still exclude reserved MMIO, RAM, firmware, other PCI devices and the display.
  Early refinement measures unresolved assigned endpoint resources before
  drivers start, instead of letting a guessed maximum size consume the aperture.
* Linux `intel_lpss_init_dev` resets the block and programs REMAP_ADDR from its
  assigned BAR. Releasing reset alone omitted an essential initialization step.
* GPIO PADBAR is dynamic, and a Linux-configured pin does not prove that native
  firmware configured it. Use read-only GPIO only when suitable; otherwise poll
  without requiring an interrupt pin. PS/2 failure must not suppress I2C input.
* DesignWare must abort a held-SCL transaction before disabling and wait for
  master activity to clear after STOP. FIFO counts alone do not prove completion.
* HID reserved bytes, optional feature reads and reset ACK timeout are not
  unconditional attachment failures upstream. Use the two-byte reset ACK path,
  configure surface/button enablement, and drop bad reports without detaching.

Polling owns no interrupt route: INTx, MSI and MSI-X are disabled. Bounded
transport recovery releases contacts before retrying. Memory bounds, malformed
descriptor checks and device identity checks remain enforced. Controller/HID
initialization and user-tested pointer movement/clicks have passed on the HP.
Gestures, scrolling and other pointer transports remain separate acceptance.

This is a small native implementation informed by the following upstream logic;
it is not a wholesale Linux driver port. Copies plus content hashes are retained
in `build/x86_64-touchpad/reference/` and `reference-sources.json`.

* [Linux HID-over-I2C core](https://github.com/torvalds/linux/blob/master/drivers/hid/i2c-hid/i2c-hid-core.c):
  30byte descriptor, unaddressed input reads, zero-length reset completion,
  power delay, GET/SET_REPORT framing.
* [Linux HID ACPI glue](https://github.com/torvalds/linux/blob/master/drivers/hid/i2c-hid/i2c-hid-acpi.c):
  HID descriptor register from HID `_DSM`, ACPI power ownership.
* [Linux DesignWare master](https://github.com/torvalds/linux/blob/master/drivers/i2c/busses/i2c-designware-master.c)
  and [v6.12 common](https://github.com/torvalds/linux/blob/v6.12/drivers/i2c/busses/i2c-designware-common.c):
  FIFO reservation, STOP/repeated START, timing formulas and RX hold workaround.
* [Linux v6.12 Intel LPSS PCI](https://github.com/torvalds/linux/blob/v6.12/drivers/mfd/intel-lpss-pci.c),
  [core](https://github.com/torvalds/linux/blob/v6.12/drivers/mfd/intel-lpss.c),
  [Meteor Lake pinctrl](https://github.com/torvalds/linux/blob/v6.12/drivers/pinctrl/intel/pinctrl-meteorlake.c)
  and [Intel GPIO core](https://github.com/torvalds/linux/blob/v6.12/drivers/pinctrl/intel/pinctrl-intel.c):
  exact device clock/reset, GPIO community and raw input definitions.
* [OpenBSD dwiic](https://github.com/openbsd/src/blob/master/sys/dev/ic/dwiic.c),
  [registers](https://github.com/openbsd/src/blob/master/sys/dev/ic/dwiicreg.h),
  [PCI](https://github.com/openbsd/src/blob/master/sys/dev/pci/dwiic_pci.c),
  [LPSS registers](https://github.com/openbsd/src/blob/master/sys/dev/pci/lpssreg.h)
  and [ihidev](https://github.com/openbsd/src/blob/master/sys/dev/i2c/ihidev.c):
  polling path, firmware timing preservation, reset release ordering and HID framing.
* [Linux PCI discovery](https://github.com/torvalds/linux/blob/master/drivers/pci/probe.c)
  and [OpenBSD PCI BAR mapping](https://github.com/openbsd/src/blob/master/sys/dev/pci/pci_map.c):
  early decode suppression, per-DWORD size/read/restore, size-mask interpretation
  and exact resource inventory before driver attachment.
