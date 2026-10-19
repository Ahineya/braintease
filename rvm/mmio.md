# Ripple VM Memory-Mapped I/O Documentation

## Overview

The Ripple VM implements a memory-mapped I/O (MMIO) system with a dedicated 32-word header at bank 0, addresses 0-31. This provides efficient access to I/O devices, random number generation, and display control without requiring system calls or special instructions.

For the `DEVICE_*` interface, start with the [submission sequence](#device-command-abi-version-1),
[worked descriptor example](#worked-example-a-capabilities-request-in-memory),
and [complete C example](#complete-c-example-using-the-helpers).

## Memory Layout

### MMIO Header (Bank 0, Words 0-31)

| Address | Name                  | R/W | Description                                           |
|---------|-----------------------|-----|-------------------------------------------------------|
| 0       | `HDR_TTY_OUT`         | W   | TTY output (low 8 bits written to stdout)             |
| 1       | `HDR_TTY_STATUS`      | R   | TTY status (bit 0: ready flag)                        |
| 2       | `HDR_TTY_IN_POP`      | R   | Pop and read next input byte                          |
| 3       | `HDR_TTY_IN_STATUS`   | R   | Input status (bit 0: has byte available)              |
| 4       | `HDR_RNG`             | R   | Read next PRNG value (auto-advances)                  |
| 5       | `HDR_RNG_SEED`        | R/W | RNG seed (low 16 bits)                                |
| 6       | `HDR_DISP_MODE`       | R/W | Display mode (0=OFF, 1=TTY, 2=TEXT40, 3=RGB565, 4=RGB565_EXT) |
| 7       | `HDR_DISP_STATUS`     | R   | Display status (bit 0: ready, bit 1: flush done)      |
| 8       | `HDR_DISP_CTL`        | R/W | Display control (bit 0: enable, bit 1: clear)         |
| 9       | `HDR_DISP_FLUSH`      | W   | Trigger display flush (write non-zero)                |
| 10 | `HDR_MATH_STACK` | R/W | RVM: read ABI 1; write packet offset in current SB |
| 11–15 | Reserved | R/W | Reads zero; writes ignored. |
| 16      | `HDR_DISP_RESOLUTION` | R/W | Display resolution for RGB565 (hi8=width, lo8=height) |
| 17      | `HDR_STORE_BLOCK`     | W   | Select current storage block (0-65535)                |
| 18      | `HDR_STORE_ADDR`      | W   | Select word address within block (0-32767)            |
| 19      | `HDR_STORE_DATA`      | R/W | Data register: read/write 16-bit word at (block, addr)       |
| 20      | `HDR_STORE_CTL`       | R/W | Storage control (busy/dirty/commit bits)              |
| 21      | `HDR_IRQ_STATUS`      | R/W | Pending IRQ flags (write ORs into pending)            |
| 22      | `HDR_IRQ_ENABLE`      | R/W | IRQ enable mask                                       |
| 23      | `HDR_IRQ_BUSY`        | R/W | R: in-service (0/1); W: nonzero acknowledges          |
| 24      | `HDR_IRQ_VECTOR_BANK` | R/W | Handler PCB                                           |
| 25      | `HDR_IRQ_VECTOR_OFF`  | R/W | Handler PC                                            |
| 26      | `HDR_IRQ_CAUSE`       | R   | 0=none, else 1 + bit index of in-service IRQ          |
| 27      | `MMIO_DEVICE_DESC_BANK` | R/W | Device descriptor bank                            |
| 28      | `MMIO_DEVICE_DESC_OFF`  | R/W | Device descriptor cell offset                     |
| 29      | `MMIO_DEVICE_COMMAND`   | W   | Submit device command                             |
| 30      | `MMIO_DEVICE_STATUS`    | R   | Device command result                             |
| 31      | `MMIO_DEVICE_VERSION`   | R   | Device ABI version (1)                            |

### TEXT40 VRAM (Bank 0, Words 32-1031)

- **Location**: Words 32-1031 (1000 words total)
- **Layout**: 40x25 character cells
- **Format**: Each word contains: `(attribute << 8) | ascii_char`
  - Low byte: ASCII character code
  - High byte: Attributes: bg and fg color, each 4 bits (16 colors total)

### General Memory (Bank 0, Word 1032+)

Regular data memory starts at word 1032, after the VRAM region.

## Device Details

### TTY I/O

**Output (HDR_TTY_OUT)**
- Write-only register at address 0
- Low 8 bits are sent to stdout immediately
- Sets TTY_STATUS busy flag temporarily (currently instant ready)

**Status (HDR_TTY_STATUS)**
- Read-only register at address 1
- Bit 0: Ready flag (1=ready to accept output, 0=busy)

**Input (HDR_TTY_IN_POP)**
- Read-only register at address 2
- Reading pops one byte from input buffer
- Returns 0 if buffer is empty

**Input Status (HDR_TTY_IN_STATUS)**
- Read-only register at address 3
- Bit 0: Has byte flag (1=byte available, 0=buffer empty)

### Random Number Generator

**RNG (HDR_RNG)**
- Read-only register at address 4
- Each read advances the PRNG state
- Returns a 16-bit pseudorandom value
- Uses Linear Congruential Generator (LCG): `next = (1664525 * prev + 1013904223) mod 2^32`

**RNG Seed (HDR_RNG_SEED)**
- Read/Write register at address 5
- Controls low 16 bits of RNG seed
- Writing sets the seed for reproducible sequences

### Storage Device

**Overview**
- Persistent block storage device with 4 GiB total capacity
- 65,536 blocks × 65,536 bytes per block = 4 GiB
- Lazy initialization: blocks are only allocated when accessed
- Backed by `~/.RippleVM/disk.img` sparse file

**Storage Block (HDR_STORE_BLOCK)**
- Write-only register at address 17
- Selects active block number (0-65535)
- All subsequent operations apply to this block

**Storage Address (HDR_STORE_ADDR)**
- Write-only register at address 18
- Selects a 16-bit word address within the current 64-KiB block (0-32767)
- Auto-increments after each HDR_STORE_DATA access
- Addresses 32768-65535 read zero and ignore writes; the cursor wraps after 65535
- Wrapping does not advance the selected block

**Storage Data (HDR_STORE_DATA)**
- Read/Write register at address 19
- Read: Returns a 16-bit little-endian word at (block, word address)
- Write: Updates both octets of that word with all 16 bits of the value
- Auto-increments HDR_STORE_ADDR after each operation

**Storage Control (HDR_STORE_CTL)**
- Read/Write register at address 20
- Control bits:
  - Bit 0 (BUSY): Read-only, 1 if VM is processing operation
  - Bit 1 (DIRTY): Read-only, 1 if current block has uncommitted writes
  - Bit 2 (COMMIT): Write-only, writing 1 commits current block
  - Bit 3 (COMMIT_ALL): Write-only, writing 1 commits all dirty blocks
  - Bit 4 (ERROR): Sticky host I/O error; write one to clear
  - Bits 15-5: Reserved (read as 0)

### Interrupt Controller

IRQ ABI **version 2** uses the same MMIO addresses 21–26, with a separate CPU
continuation and atomic IRET. The lowest enabled pending source is taken before
instruction execution. Entry preserves RA/RAB; BUSY inhibits nesting. The active
source is removed from STATUS and reported through CAUSE (1 + bit index).

- STATUS reads pending events; writes OR new events into the pending mask.
- ENABLE selects eligible sources.
- BUSY reads in-service state. Writing nonzero acknowledges the active event;
  it keeps BUSY set until IRET. Zero and acknowledgement outside a handler are ignored.
- VECTOR_BANK/OFF select the handler.
- CAUSE is read-only and remains latched until IRET.

Handlers preserve any general registers they modify, acknowledge, restore those
registers, and execute IRET. The return atomically restores saved PC/PCB, enable
mask, SB/SP/FP, and clears BUSY. An unacknowledged source is requeued. Events raised
during the handler, including a repeated event of the active source, remain pending.
RET is an ordinary C return and cannot substitute for IRET.

IRQ_SW is bit 0 and IRQ_TIMER is bit 1. CSRR/CSRW expose separate CPU return state,
an optional interrupt stack, and an instruction-count timer. They do not displace
the device mailbox at 27–31. The full contract, CSR numbers, C helpers, and binary
migration requirements are in [stacks-and-interrupts.md](../docs/arch/stacks-and-interrupts.md).

### Keyboard and pointer input

Input uses the DEVICE_* mailbox in every display mode. Registers 11–15 are
reserved; register 10 is the atomic math stack service. The C directional helpers read DEVICE_INPUT_STATE using USB HID usages.
TEXT40 terminal key presses become short pulses; windowed RGB565 input provides
press/release, pointer motion, buttons, wheel, focus and capture events.

### Display System

**Display Mode (HDR_DISP_MODE)**
- Read/Write register at address 6
- Values:
  - 0: Display OFF
  - 1: TTY passthrough mode
  - 2: TEXT40 mode (40x25 character display)
  - 3: RGB565 mode (graphics display)
  - 4: RGB565_EXT mode (banked framebuffer via device command ABI)

**Display Status (HDR_DISP_STATUS)**
- Read-only register at address 7
- Bit 0: Ready flag
- Bit 1: Flush done flag

**Display Control (HDR_DISP_CTL)**
- Read/Write register at address 8
- Bit 0: Enable display
- Bit 1: Clear VRAM (edge-triggered, auto-clears)

**Display Flush (HDR_DISP_FLUSH)**
- Write-only register at address 9
- Writing non-zero triggers display update
- Sets flush_done flag when complete
- In mode 3 (RGB565), swaps the front and back framebuffers
- In mode 4 (RGB565_EXT), use the `PRESENT` command below

**Display Resolution (HDR_DISP_RESOLUTION)**
- Read/Write register at address 16
- Used for legacy RGB565 mode 3 only
- Format: high 8 bits = width, low 8 bits = height
- Must be set BEFORE switching to RGB565 mode
- Maximum resolution depends on bank size: `(bank_size - 32) / 2` pixels total

### RGB565 Graphics Mode

**Overview**
- 16-bit color per pixel (5 bits red, 6 bits green, 5 bits blue)
- Double-buffered for smooth animation
- Resolution configurable up to bank size limits

**Setup Procedure**
1. Set desired resolution at HDR_DISP_RESOLUTION (address 16)
2. Set display mode to 3 (RGB565) at HDR_DISP_MODE (address 6)
3. If resolution doesn't fit in bank, VM will halt

**Memory Layout in RGB565 Mode**
- Words 0-31: MMIO headers (unchanged)
- Words 32 to 32+WxH-1: Front buffer (displayed)
- Words 32+WxH to 32+2xWxH-1: Back buffer (for drawing)

**RGB565 Color Format**
```
Bit:  15 14 13 12 11 | 10 9 8 7 6 5 | 4 3 2 1 0
      R  R  R  R  R  | G  G G G G G | B B B B B
```

**Drawing Workflow**
1. Write pixels to back buffer memory addresses
2. Write non-zero to HDR_DISP_FLUSH to swap buffers
3. Back buffer becomes visible, old front buffer becomes new back buffer

## Implementation Details

### Device command ABI version 1

Addresses 27–31 form a versioned mailbox. A command descriptor is a contiguous
sequence of 16-bit cells in guest data memory; descriptor and framebuffer pointers
are `(bank, offset)` pairs. Crossing a bank boundary continues at offset zero in
the next bank. Both lengths and strides count cells, not eight-bit octets. One
descriptor cell is `sizeof(unsigned short) == 1` in Ripple C.

To submit a command, write its descriptor bank to address 27, descriptor offset
to 28, then command ID to 29. Read the result at address 30. The ABI version at
address 31 is read-only and currently returns 1. Old hosts return zero at this
previously reserved address; check it before using the interface.

A descriptor is a **request-and-reply record in guest RAM**. The guest chooses
where to put it: a stack array, a global, or an allocated buffer all work. The
host reads its input fields and writes results into its output fields. Addresses
27–31 hold the pointer, command and status; they do not hold the descriptor itself.

The sequence is:

1. Check `DEVICE_VERSION` at bank 0, address 31.
2. Fill the descriptor in ordinary guest RAM, including version and length.
3. Write its bank and offset to bank-0 addresses 27 and 28.
4. Write the command ID to bank-0 address 29. **This write submits the request.**
5. Read status at address 30. On success, consume the descriptor's output fields.

Writing the pointer registers alone does nothing, and the host does not watch the
descriptor for changes. Submit each request explicitly. Commands complete before
the next guest instruction; `NEXT_TICK` can suspend the guest while waiting for a
host grant. Keep the descriptor and any referenced buffer alive until completion.

The descriptor-pointer and status registers are shared by all device commands.
Finish each submission and read its result before another caller uses the
mailbox. Multitasking or interrupt-handler callers must serialize the entire
transaction; the C helpers do not provide that synchronization. Individual
devices retain their own state between submissions. See the planned networking
device's [mailbox-sharing rules](../docs/mmio.networking.md#sharing-the-mailbox-with-other-devices).

The pointer can select any valid range in guest data RAM, subject to bounds and
bank-offset validation; it cannot select the MMIO header or host process memory.
The host interprets the memory according to the selected command. For example,
`PRESENT` reads a pixel buffer, while `CAPS` writes query results into the
descriptor. The command ID selects a built-in host operation; the descriptor
holds its arguments and results.

Offsets count cells within the configured bank size. With 64,000-cell banks,
`(2, 100)` means absolute guest cell `2 * 64000 + 100`. A four-cell descriptor
starting at `(1, 63998)` occupies `(1, 63998)`, `(1, 63999)`, `(2, 0)`, `(2, 1)`.
This continuation differs from packed C pointer encoding: a pointer cast to
`unsigned long` stores bank in the high 16 bits and offset in the low 16 bits.
The C helpers handle that encoding for you.

Every descriptor starts with version 1 and its exact length in cells; larger or
smaller lengths are rejected. Version-1 layouts are listed below in cell order.
Multiword integers use the low cell first.

| Command | ID | Length | Descriptor cells |
|---------|----|--------|------------------|
| PRESENT | 1 | 8 | version, length, width, height, stride, buffer bank, buffer offset, flags=0 |
| CAPS | 2 | 4 | version, length, capability bits (out), key count (out) |
| NEXT_TICK | 3 | 8 | version, length, tick low (out), tick high (out), milliseconds low (out), milliseconds high (out), Hz (out), reserved=0 |
| GET_TIME | 4 | 8 | same as NEXT_TICK |
| INPUT_READ | 5 | 10 | version, length, capacity, event buffer bank, offset, count (out), overflow (out), sequence low/high (out), reserved=0 |
| INPUT_STATE | 6 | 28 | version, length, overflow, sequence low/high, requested pointer mode, buttons, flags, X low/high, Y low/high, 16 held-key bitmap cells (all after length are outputs) |
| INPUT_CONFIGURE | 7 | 4 | version, length, pointer mode (0 absolute, 1 relative), reserved=0 |
| HOST_MONOTONIC | 8 | 10 | version, length, seconds (four output cells), nanoseconds (two output cells), resolution_ns (two output cells) |
| HOST_REALTIME | 9 | 10 | same layout as HOST_MONOTONIC |
| HOST_IDLE | 10 | 4 | version, length, requested milliseconds (0–10), reserved zero |
| HOST_TERMINAL | 11 | 4 | version, length, rows, columns (payload zero on submission) |
| HOST_RANDOM | 12 | 6 | version, length, output bank, output offset, count (1–256), reserved zero |
| CRYPTO | 13 | 6 | version, length, request bank, request offset, request cells (2056), reserved zero |
| TTY_WRITE | 14 | 8 | version, length, source bank/offset, octet count, reserved zero, completed out, reserved zero |
| MATH | 15 | 16 | version, length, operation, reserved zero, A (4 cells), B (4 cells), result (4 cells) |

#### Worked example: a capabilities request in memory

Assume bank 2, offsets 100–103 are available guest RAM. First write this record:

| Guest location | Before submission | Meaning | After successful `CAPS` on RVM/native hosts |
|----------------|-------------------|---------|-----------------------------------------|
| `(2, 100)` | 1 | descriptor version | 1 |
| `(2, 101)` | 4 | descriptor length, including these first two cells | 4 |
| `(2, 102)` | 0 | capability bits, output | backend-dependent bitmask |
| `(2, 103)` | 0 | key count, output | 256 |

Then perform these bank-0 MMIO operations, in order:

```text
write address 27 = 2      # descriptor bank
write address 28 = 100    # descriptor offset
write address 29 = 2      # CAPS command ID; host processes the request now
read  address 30          # 0 means success
```

The host obtains the descriptor address from registers 27/28, reads its header,
validates the four-cell range, and writes results at `(2, 102)` and `(2, 103)`.
Read those cells only after checking status. Test capability bits with a mask;
hosts may return additional bits; do not compare the whole value to a constant.

Here is a complete Ripple C program doing the same operation with a stack array
instead of a hard-coded address. `volatile` ensures the descriptor's memory
accesses are preserved around the host operation.

```c
#include <mmio.h>
#include <vm_devices.h>
#include <stdio.h>

int main(void) {
    volatile unsigned short descriptor[4];
    unsigned long address;
    unsigned short status;

    if (mmio_read(MMIO_DEVICE_VERSION) != VM_DEVICE_ABI_VERSION) return 1;
    descriptor[0] = VM_DEVICE_ABI_VERSION;
    descriptor[1] = 4;
    descriptor[2] = 0;
    descriptor[3] = 0;
    address = (unsigned long)descriptor;
    mmio_write(MMIO_DEVICE_DESC_BANK, (unsigned short)(address >> 16));
    mmio_write(MMIO_DEVICE_DESC_OFF, (unsigned short)address);
    mmio_write(MMIO_DEVICE_COMMAND, VM_DEVICE_CAPS);
    status = mmio_read(MMIO_DEVICE_STATUS);
    if (status != VM_DEVICE_OK) return 2;
    printf("caps=%u keys=%u\n", descriptor[2], descriptor[3]);
    return 0;
}
```

In application code, `vm_devices_caps(&caps)` performs this construction,
submission and output extraction for you.

#### Worked example: descriptor and framebuffer are separate

Suppose a descriptor at `(2, 200)` contains these eight cells:

```text
[1, 8, 2, 2, 3, 3, 50, 0]
       |  |  |  |   |
       |  |  |  |   +-- framebuffer offset 50
       |  |  |  +------ framebuffer bank 3
       |  |  +--------- row stride 3 cells
       |  +------------ height 2
       +--------------- width 2
```

The framebuffer at `(3, 50)` contains:

```text
offset:  50      51      52        53      54
value:   0xf800  0x07e0  padding   0x001f  0xffff
pixel:   red     green   skipped   blue    white
```

Select mode 4 by writing 4 to bank-0 address 6. Write descriptor bank 2 to
address 27, offset 200 to address 28, and command ID 1 (`PRESENT`) to address 29.
On success, the host has copied a 2×2 frame: red/green above blue/white. Stride
is the distance between row starts, so the padding cell is not displayed. The
guest may then reuse the framebuffer; presentation has not advanced the clock.

#### Status and capabilities

| Status | Meaning |
|--------|---------|
| 0 | success |
| 1 | unknown command |
| 2 | unsupported descriptor version |
| 3 | invalid descriptor or fields |
| 4 | guest-memory bounds error |
| 5 | wrong display mode |
| 6 | input queue overflow during a tick grant |
| 7 | unsupported device on this backend |
| 8 | bounded queue would block |
| 9 | invalid device state |
| 10 | host device I/O error |
| 0x8000 | pending host tick grant |

`CAPS` includes bits 0–8 in RVM and the RCC native runtime: extended
RGB565, logical clock, input events,
held-key state, independent live-input polling (`VM_CAP_INPUT_POLL`, 0x0010),
Ethernet networking (`VM_CAP_NETWORK`, 0x0020), pointer input (`VM_CAP_POINTER`,
0x0040), PCM audio (`VM_CAP_PCM`, 0x0080), and OPL3 music (`VM_CAP_MUSIC`, 0x0100).
RBF includes bits 0–6 and leaves audio unsupported. Additional capability bits
report bulk RAM transfers (0x0400), attached storage (0x0200), and code uploads
(0x0800); availability depends on the backend and CPU image. Host monotonic and
calendar clocks use `VM_CAP_HOST_CLOCK` (0x1000) on RVM, RBF and native C.
See [audio](../docs/arch/audio.md). Key count is 256. Ignore unknown capability
bits and check status before consuming output fields.

Networking uses additional command IDs in this same mailbox: `NET_INFO` 0x0100
(18 cells), `NET_TX` 0x0101 (8), `NET_RX` 0x0102 (9), and `NET_STATS` 0x0103 (27).
See the [network wire formats and examples](../docs/mmio.networking.md#commands-and-wire-format).
It needs no separate MMIO addresses. Packet transfers copy one eight-bit octet
per guest cell and do not advance time, consume keyboard events or publish frames.

#### Bulk storage and memory transfers

Commands 0x0300–0x0303 provide bounded disk read/write, RAM copy/fill, and Harvard
instruction uploads through this mailbox. See the [wire contract and C examples](../docs/arch/bulk-io.md).
Command 0x0304 (`MEMORY_INFO`) uses an eight-cell descriptor to report RAM-cell
and instruction capacities, bank size, and reserved zero. Its capacities reflect
RVM's configurable `--code-memory` budget or RBF's exported code reservation;
the query allocates nothing and does not advance time.
Disk offsets count octets; RAM lengths count cells; code lengths count instructions.
Bulk and legacy storage share cached dirty data and durability barriers. No guest
ISA instructions or MMIO addresses are added.

#### Extended RGB565 presentation

Select display mode 4, then submit `PRESENT` with positive width and height,
`stride >= width`, and zero flags. Dimensions and stride are 16-bit values.
Pixels are row-major RGB565 cells; row `y` begins at `buffer + y * stride`.
The host validates the entire accessed range and copies the frame before the
command completes. Thus the guest can immediately reuse or free the framebuffer.
Stride padding is not displayed. The framebuffer may span banks, allowing
320×200 output without changing the legacy bank-0 layout.

Each successful `PRESENT` publishes exactly one frame. Mode 3 retains its existing
double-buffer swap at `HDR_DISP_FLUSH`. Neither presentation path advances logical
time. Screenshot capture observes the completed frame, rather than an arbitrary
wall-clock instant or partially written back buffer.

#### Logical clock

`NEXT_TICK` requests exactly one simulation tick. The command can become pending
while the host waits, but the host completes its descriptor and status before
executing the next guest instruction. Guest code does not spin on status.
`GET_TIME` only reads: repeated reads and framebuffer presentations cannot advance
time. Tick and milliseconds are unsigned 32-bit values, with milliseconds derived
from tick count and Hz. The default frequency is 35 Hz.

Host policies can pace tick grants to real time, grant them immediately for fast
tests, or wait for explicit steps. All policies expose the same guest clock.
CPU frequency throttling is a separate host setting and does not define the
simulation clock. A Doom port should request ticks in its game loop and implement
its time callback using `GET_TIME`; rendering must not become the clock source.

#### Host clocks

`HOST_MONOTONIC` reads elapsed host time from the first monotonic read after
device reset. It includes host pauses and is independent of CPU instruction
counts, runner pacing and the logical game clock. `HOST_REALTIME` reads Unix
UTC time from the host calendar; calendar corrections may move it backward.
The latter reports I/O error if the host date precedes the Unix epoch.

Both commands are synchronous and return unsigned 64-bit whole seconds,
unsigned 32-bit nanoseconds, and unsigned 32-bit resolution in nanoseconds.
Values are quantized to 1 ms and resolution is 1,000,000 ns. All eight cells
after the header are outputs. Invalid headers or bounds leave RAM unchanged.
`vm_host_monotonic()` and `vm_host_realtime()` return `VmHostTime`, whose
`seconds_low`/`seconds_high` fields avoid requiring a 64-bit RCC integer type.
As with other mailbox operations, callers must serialize submissions against
interrupt handlers. No simulation tick, input event or IRQ is generated.

Device checkpoints save elapsed monotonic milliseconds without serializing a
host `Instant`. Restore anchors the saved value to the new host instant, so
time spent saved is excluded. Old checkpoints without clock state start at
zero. Realtime always uses the current host calendar after restore. Host-clock
reads are not deterministic replay inputs: guests requiring deterministic
execution should continue to use `GET_TIME` and `NEXT_TICK` exclusively.

#### Keyboard and pointer events and state

`INPUT_READ` drains up to `capacity` ordered events (1–64). Its output buffer
holds `capacity * 20` cells; buffers and descriptors may cross banks but must be
valid guest RAM and must not overlap. Every range and descriptor field is checked
before any event is consumed or output is written. A successful empty read has
count zero. The output sequence is the current snapshot boundary, which can be
newer than the final event in a partial batch. Sequence numbers wrap modulo 2^32.

Each event occupies 20 cells; 32-bit fields use low/high cells in that order:

| Cells | Field |
|-------|-------|
| 0 | type: key=1, motion=2, button=3, wheel=4, enter/leave=5, focus=6, capture=7, window close request=8 |
| 1 | flags: DOWN=1, RELATIVE=2, INSIDE=4, FOCUSED=8, CAPTURED=16 |
| 2 | code: USB HID usage for keys, button ID for button transitions, otherwise zero |
| 3 | held button bitmap after this event |
| 4–5 | unsigned sequence |
| 6–7 | low 32 bits of logical tick |
| 8–9, 10–11 | signed X, Y position |
| 12–13, 14–15 | signed movement delta X, Y (zero for other event types) |
| 16–17, 18–19 | signed wheel X, Y in 16.16 lines (zero for other types) |

Key IDs are USB HID usages 0–255: A–Z=4–29, Enter=40, Escape=41, Space=44,
right/left/down/up=79/80/81/82, modifiers=224–231. Held keys persist until release;
auto-repeat does not generate extra presses. Button IDs are left=0, right=1,
middle=2, auxiliary=3–15; auxiliary numbering follows host conventions.

Absolute X/Y count framebuffer pixels from its top left (X right, Y down).
They are signed and may lie outside the framebuffer in the window's letterbox
or after leaving it. INSIDE means inside the host window, independently of those
coordinates. Button events carry the click position and resulting button bitmap.
Relative events have RELATIVE set: deltas count raw device units with the same
axis directions, and their virtual X/Y accumulate with signed saturation. Wheel
X is positive right and Y positive up. Pixel scrolling is normalized using 40
logical window pixels per line, preserving fractional lines as signed 16.16.

`INPUT_CONFIGURE` requests absolute or relative mode. Relative mode asks a focused
window to grab and hide the cursor (locked, or confined where locking is not
supported). Capture is asynchronous: CAPTURE events and the CAPTURED snapshot
flag report actual host acceptance, including failure to capture. Absolute mode
releases it. Focus loss releases held keys, buttons and capture in order. Relative
mode is requested again on refocus. Headless hosts do not claim live capture.

With `VM_CAP_INPUT_POLL`, READ and STATE collect live events at the current logical
tick, including zero, without advancing time or requiring a presentation. Replay
and controller events are delivered at tick grants, never early by polling.
Keyboard, button, motion, wheel, enter, focus, capture and close share one ordered queue.
With RVM `--visual --guest-window-close`, a host close hides the window and
sends a close request without stopping the VM. Focus/input releases precede it;
a new display lease may reopen the window. This policy is opt-in.

`INPUT_STATE` reports the current sequence boundary, requested pointer mode,
button bitmap, flags, position, and all 256 keys. Key `k` is bit `(k & 15)` of
cell `12 + (k >> 4)`. A normal snapshot leaves queued events intact. After queue
overflow, the snapshot clears sticky overflow and discards stale queued events;
close requests are retained because a held-state snapshot cannot recover them.
Replace application state with this authoritative snapshot. State continues to
update while events are lost. Host overflow establishes a fresh snapshot boundary;
sequence gaps do not measure the exact number of lost host transitions.

The queue holds 256 events. READ reports overflow without clearing it. A tick
grant that overflows reports status 6; automated runs fail on observed overflow.
The ABI remains version 1; no alternate input layouts are supported.

#### C interface and backend compatibility

Include `<vm_devices.h>` and link the runtime. Its wrappers construct descriptors
on the stack, extract the compiler's packed bank/offset pointer representation,
submit commands, check status, and copy successful outputs into typed structs.
Use `vm_devices_version()` for discovery, `vm_devices_caps()` for capabilities,
`vm_present_rgb565()` for output, `vm_next_tick()` / `vm_get_time()` for time,
and `vm_input_read()` / `vm_input_state()` / `vm_input_configure()` for input.

The instruction set is unchanged. The Rust VM, Brainfuck host and native
runtime share this device protocol and live-input collection. The Brainfuck
CPU still executes guest instructions in ordinary Brainfuck. See
[`docs/doom-platform.md`](../docs/doom-platform.md) for the current port boundary.

#### Complete C example using the helpers

This one-shot program queries capabilities, polls input before any tick grant,
advances one tick, and presents a 2×2 frame. The helpers allocate descriptors on
the stack; `pixels` is a separate guest-memory buffer. No manual bank calculation
is needed. The program exits after presenting; an interactive application would
repeat input, simulation and rendering according to its own loop policy.

```c
#include <mmio.h>
#include <vm_devices.h>
#include <stdio.h>

static unsigned short pixels[4] = {0xf800U, 0x07e0U, 0x001fU, 0xffffU};

int main(void) {
    VmDeviceCaps caps;
    VmClock clock;
    VmInputEvent event;
    VmInputBatch batch;
    VmInputState keys;
    unsigned short needed = VM_CAP_RGB565_EXT | VM_CAP_CLOCK
        | VM_CAP_INPUT_EVENTS | VM_CAP_INPUT_STATE | VM_CAP_INPUT_POLL;

    if (vm_devices_version() != VM_DEVICE_ABI_VERSION) return 1;
    if (vm_devices_caps(&caps) != VM_DEVICE_OK) return 2;
    if ((caps.bits & needed) != needed) return 3;
    display_set_mode(DISP_MODE_RGB565_EXT);
    display_enable();

    if (vm_get_time(&clock) != VM_DEVICE_OK) return 4;
    printf("before: tick=%lu ms=%lu\n", clock.tick, clock.ms);

    /* Independent input polling: no NEXT_TICK required. */
    if (vm_input_state(&keys) != VM_DEVICE_OK) return 5;
    printf("up=%d\n", vm_key_is_down(&keys, VM_KEY_UP));
    for (;;) {
        if (vm_input_read(&event, 1, &batch) != VM_DEVICE_OK) return 6;
        if (batch.overflow) {
            /* Recover the authoritative bitmap and discard stale events. */
            if (vm_input_state(&keys) != VM_DEVICE_OK) return 7;
            printf("resync up=%d\n", vm_key_is_down(&keys, VM_KEY_UP));
            break;
        }
        if (!batch.count) break;
        if (event.type == VM_INPUT_KEY)
            printf("key=%u down=%u tick=%lu\n", event.code,
                   event.flags & VM_INPUT_DOWN, event.tick);
    }

    if (vm_next_tick(&clock) != VM_DEVICE_OK) return 8;
    printf("after: tick=%lu ms=%lu\n", clock.tick, clock.ms);
    if (vm_present_rgb565(pixels, 2, 2, 2) != VM_DEVICE_OK) return 9;
    return 0;
}
```

With a fresh, headless host, no replay and the default 35 Hz clock, output is:

```text
before: tick=0 ms=0
up=0
after: tick=1 ms=28
```

`GET_TIME` and input polling leave time unchanged. Only `NEXT_TICK` advances it;
paced execution may wait, while unthrottled execution grants it immediately.
Replay events scheduled for tick 1 become available after that grant, so call
`vm_input_read()` again afterwards to consume them. An ordinary `INPUT_STATE`
snapshot leaves queued events intact; after overflow it clears stale events and
the overflow flag. An application maintaining controls must replace its own
held-key state from that recovery snapshot.

### MMIO Read Handling

With the MMU enabled, ordinary user accesses translate and check permissions
before physical MMIO routing. The atomic math register is the explicit exception.
Unprotected/supervisor memory reads route in this order:
1. Bank 0, addresses 0–31: MMIO header registers.
2. In legacy RGB565 mode 3, bank-0 front/back framebuffer addresses: display
   buffer reads, including addresses below the old TEXT40 boundary at 1032.
3. All other addresses: regular guest memory. TEXT40 VRAM is ordinary bank-0
   memory; mode 4 framebuffers are ordinary memory copied by `PRESENT`.

```rust
fn handle_mmio_read(&mut self, addr: usize) -> Option<u16> {
    match addr {
        HDR_TTY_OUT => Some(0),  // Write-only
        HDR_TTY_STATUS => Some(if self.output_ready { TTY_READY } else { 0 }),
        HDR_TTY_IN_POP => {
            let value = self.input_buffer.pop_front().unwrap_or(0) as u16;
            self.memory[HDR_TTY_IN_POP] = value;
            Some(value)
        },
        HDR_TTY_IN_STATUS => Some(if !self.input_buffer.is_empty() { TTY_HAS_BYTE } else { 0 }),
        HDR_RNG => {
            self.rng_state = self.rng_state.wrapping_mul(1664525).wrapping_add(1013904223);
            let value = (self.rng_state >> 16) as u16;
            self.memory[HDR_RNG] = value;
            Some(value)
        },
        // ... other MMIO addresses
        _ => None  // Not MMIO
    }
}
```

### MMIO Write Handling

Bank-0 addresses 0–31 are MMIO writes. In legacy RGB565 mode 3, writes in its
framebuffer region go to the display device; addresses beyond the buffers fall
back to ordinary RAM. Other banks, TEXT40 VRAM and mode 4 framebuffers are regular
guest memory. The abbreviated handler below shows header registers only:

```rust
fn handle_mmio_write(&mut self, addr: usize, value: u16) -> bool {
    match addr {
        HDR_TTY_OUT => {
            let byte = (value & 0xFF) as u8;
            io::stdout().write_all(&[byte]);
            io::stdout().flush();
            self.output_buffer.push_back(byte);
            true
        },
        HDR_DISP_CTL => {
            if value & DISP_CLEAR != 0 {
                // Clear VRAM
                for i in TEXT40_BASE_WORD..=TEXT40_LAST_WORD {
                    self.memory[i] = 0;
                }
            }
            if value & DISP_ENABLE != 0 {
                self.display_enabled = true;
            }
            true
        },
        // ... other MMIO addresses
        _ => false  // Not MMIO
    }
}
```

### Memory Access Instructions

LOAD and STORE retain their existing encodings. Their host memory routing checks
the 32-cell MMIO header first, then any legacy RGB565 framebuffer mapping, then
ordinary memory. Device descriptors and mode 4 framebuffers do not add an
instruction or a special pixel-store operation.

## Usage Examples

### Basic TTY Output
```asm
; Print 'A' to stdout
LI    A0, 'A'
LI    T0, 0        ; Bank 0
LI    T1, 0        ; Address 0 (HDR_TTY_OUT)
STORE A0, T0, T1
```

### Reading Input
```asm
; Check for input and read if available
LI    T0, 0        ; Bank 0
LI    T1, 3        ; HDR_TTY_IN_STATUS
LOAD  T2, T0, T1
ANDI  T2, T2, 1
BEQ   T2, R0, no_input

LI    T1, 2        ; HDR_TTY_IN_POP
LOAD  A0, T0, T1   ; Read the byte
no_input:
```

### TEXT40 Display
```asm
; Initialize TEXT40 display
LI    A0, 2        ; TEXT40 mode
LI    T0, 0        ; Bank 0
LI    T1, 6        ; HDR_DISP_MODE
STORE A0, T0, T1

LI    A0, 1        ; Enable display
LI    T1, 8        ; HDR_DISP_CTL
STORE A0, T0, T1

; Write "Hi" at top-left
LI    A0, 'H'
LI    T1, 32       ; VRAM[0]
STORE A0, T0, T1

LI    A0, 'i'
LI    T1, 33       ; VRAM[1]
STORE A0, T0, T1

; Flush display
LI    A0, 1
LI    T1, 9        ; HDR_DISP_FLUSH
STORE A0, T0, T1
```

### Random Number Generation
```asm
; Get random number
LI    T0, 0        ; Bank 0
LI    T1, 4        ; HDR_RNG
LOAD  A0, T0, T1   ; Random value in A0
```

### Keyboard input

Submit INPUT_STATE as described above. The up arrow (HID usage 82) is bit 2 of
snapshot cell 17; registers 11–15 are reserved and register 10 submits math packets.

### Storage Operations
```asm
; Write data to block 42, starting at word 0
LI    A0, 42
LI    T0, 0        ; Bank 0
LI    T1, 17       ; HDR_STORE_BLOCK
STORE A0, T0, T1

LI    A0, 0
LI    T1, 18       ; HDR_STORE_ADDR
STORE A0, T0, T1

; Write "Hello" (one character per 16-bit word)
LI    A0, 'H'
LI    T1, 19       ; HDR_STORE_DATA
STORE A0, T0, T1   ; Auto-increments address

LI    A0, 'e'
STORE A0, T0, T1   ; Auto-increments address

LI    A0, 'l'
STORE A0, T0, T1   ; Auto-increments address

LI    A0, 'l'
STORE A0, T0, T1   ; Auto-increments address

LI    A0, 'o'
STORE A0, T0, T1   ; Auto-increments address

; Commit the block to disk
LI    A0, 4        ; Bit 2 = COMMIT
LI    T1, 20       ; HDR_STORE_CTL
STORE A0, T0, T1

; Read back the data
LI    A0, 42
LI    T1, 17       ; HDR_STORE_BLOCK
STORE A0, T0, T1

LI    A0, 0
LI    T1, 18       ; HDR_STORE_ADDR
STORE A0, T0, T1

LI    T1, 19       ; HDR_STORE_DATA
LOAD  A0, T0, T1   ; Read first word ('H')
LOAD  A1, T0, T1   ; Read second word ('e') (auto-increment)
LOAD  A2, T0, T1   ; Read third word ('l') (auto-increment)
LOAD  A3, T0, T1   ; Read fourth word ('l') (auto-increment)
LOAD  X0, T0, T1   ; Read fifth word ('o') (auto-increment)
```

## C Runtime Integration

The C runtime library uses these MMIO addresses for standard I/O:

```c
// putchar implementation
void putchar(int c) {
    volatile uint16_t* tty_out = (volatile uint16_t*)0;
    volatile uint16_t* tty_status = (volatile uint16_t*)1;
    
    // Wait for ready
    while ((*tty_status & 1) == 0) {
        // Spin wait
    }
    
    // Output character
    *tty_out = c & 0xFF;
}

// getchar implementation
int getchar(void) {
    volatile uint16_t* tty_in_status = (volatile uint16_t*)3;
    volatile uint16_t* tty_in_pop = (volatile uint16_t*)2;
    
    // Wait for input
    while ((*tty_in_status & 1) == 0) {
        // Spin wait
    }
    
    // Read and return byte
    return *tty_in_pop & 0xFF;
}
```

IRQ helpers live in `runtime/include/mmio.h`. Do not enable a pending STATUS bit unless a handler is installed — dispatch runs at the start of the next instruction.

```c
irq_set_vector(0, handler_pc);
irq_set_enable(IRQ_SW);
irq_raise(IRQ_SW);
irq_ack();  // in the handler: acknowledge; assembly restoration ends with IRET
```

## Design Rationale

1. **Fixed Addresses**: All MMIO addresses are fixed at compile time, eliminating runtime discovery overhead
2. **Bank 0 Only**: MMIO is only active in bank 0, simplifying implementation and preventing conflicts
3. **Minimal Header**: the 32-word header includes a versioned device mailbox at words 27–31
4. **Efficient Access**: Low addresses (0-31) are optimal for Brainfuck-generated code
5. **Backward Compatible**: Legacy MMIO_OUT and MMIO_OUT_FLAG aliases maintained at addresses 0 and 1

## Constants Reference

```rust
// MMIO Header Addresses
pub const HDR_TTY_OUT: usize       = 0;
pub const HDR_TTY_STATUS: usize    = 1;
pub const HDR_TTY_IN_POP: usize    = 2;
pub const HDR_TTY_IN_STATUS: usize = 3;
pub const HDR_RNG: usize           = 4;
pub const HDR_RNG_SEED: usize      = 5;
pub const HDR_DISP_MODE: usize     = 6;
pub const HDR_DISP_STATUS: usize   = 7;
pub const HDR_DISP_CTL: usize      = 8;
pub const HDR_DISP_FLUSH: usize    = 9;
pub const HDR_MATH_STACK: usize    = 10;
pub const HDR_DISP_RESOLUTION: usize = 16;
pub const HDR_STORE_BLOCK: usize   = 17;
pub const HDR_STORE_ADDR: usize    = 18;
pub const HDR_STORE_DATA: usize    = 19;
pub const HDR_STORE_CTL: usize     = 20;
pub const HDR_IRQ_STATUS: usize    = 21;
pub const HDR_IRQ_ENABLE: usize    = 22;
pub const HDR_IRQ_BUSY: usize      = 23;
pub const HDR_IRQ_VECTOR_BANK: usize = 24;
pub const HDR_IRQ_VECTOR_OFF: usize  = 25;
pub const HDR_IRQ_CAUSE: usize     = 26;
// Device mailbox constants live in rvm/src/devices.rs.
pub const REGISTER_DESCRIPTOR_BANK: usize   = 27;
pub const REGISTER_DESCRIPTOR_OFFSET: usize = 28;
pub const REGISTER_COMMAND: usize           = 29;
pub const REGISTER_STATUS: usize            = 30;
pub const REGISTER_VERSION: usize           = 31;

pub const IRQ_SW: u16              = 0x0001;  // STATUS/ENABLE bit 0

// TEXT40 VRAM
pub const TEXT40_BASE_WORD: usize  = 32;
pub const TEXT40_WORDS: usize      = 40 * 25;
pub const TEXT40_LAST_WORD: usize  = 1031;

// Status Bits
pub const TTY_READY: u16           = 0x0001;
pub const TTY_HAS_BYTE: u16        = 0x0001;
pub const DISP_READY: u16          = 0x0001;
pub const DISP_FLUSH_DONE: u16     = 0x0002;
pub const DISP_ENABLE: u16         = 0x0001;
pub const DISP_CLEAR: u16          = 0x0002;

// Display Modes
pub const DISP_OFF: u16            = 0;
pub const DISP_TTY: u16            = 1;
pub const DISP_TEXT40: u16         = 2;
pub const DISP_RGB565: u16         = 3;
pub const DISPLAY_MODE_EXTENDED: u16 = 4; // rvm/src/devices.rs

// Storage Control Bits
pub const STORE_BUSY: u16          = 0x0001;  // bit0
pub const STORE_DIRTY: u16         = 0x0002;  // bit1
pub const STORE_COMMIT: u16        = 0x0004;  // bit2
pub const STORE_COMMIT_ALL: u16    = 0x0008;  // bit3
```

## Future Enhancements

Networking has a [device contract and acceptance plan](../docs/mmio.networking.md):
a raw Ethernet NIC through the existing mailbox, bounded host queues, one octet
per guest cell, bidirectional host ping, and an external VPS ping stretch goal.
The Ethernet commands and local host attachment are implemented. Guest ICMP to
the external VPS's IPv4 address has passed on all three runtimes; guest DNS
remains later work.

Further device commands can extend the versioned mailbox without moving existing
MMIO addresses. Possible future devices include:
- Sound generation
- Additional network backends
- DMA controllers
- Serial communication ports

New commands must preserve existing descriptor versions and advertise their
capabilities. A command's availability in the Rust VM does not imply support in
the Brainfuck host adapter.

### Durable storage commits and errors

In RVM, `STORE_CTL` bit 2 (COMMIT) and bit 3 (COMMIT_ALL) wait for the
backing file's `sync_all`/`fsync` before acknowledging dirty blocks. A failed
write or durability barrier leaves the affected block dirty. Bit 4 is a sticky
host I/O error flag; write one to bit 4 to acknowledge it (optionally with a
retry command). Successful unrelated operations do not clear the error.
COMMIT_ALL takes precedence if both commit command bits are written.

This flushes the VM storage cache. Guest filesystem buffers still need a guest
`sync` or clean shutdown. Neither operation makes a filesystem transaction
atomic or guarantees a clean filesystem after interruption between writes.

## Cryptographic host entropy

`VM_CAP_HOST_RANDOM` (0x8000) advertises `HOST_RANDOM` (12). The descriptor
is `[1, 6, bank, offset, count, 0]`; output lengths count cells, with one
0–255 octet per cell. Complete RAM ranges may cross banks but cannot overlap
MMIO or the descriptor. Entropy is staged before copying: invalid requests
and OS failures leave guest memory unchanged. Failure returns `IO` without
a predictable fallback. It advances no simulation time and raises no IRQ.
The seeded `RNG` and `RNG_SEED` registers remain deterministic and must not
seed cryptographic operations. See [host entropy](../docs/arch/entropy.md)
for the C API, guest `/dev/urandom` and validation.

See the [crypto coprocessor contract](../docs/arch/crypto.md) for command 13 and `/dev/crypto`.

## Scalar math and atomic stack packets

Command 15 implements IEEE binary32/binary64 arithmetic, conversions and
binary64 libm. RVM also provides atomic, MMU-aware stack submission through
bank-0 address 10. Read-only CSR 28 feature bit 2 advertises user access to this
single register. Other MMIO remains protected. See the
[math contract, packet layout and runtime integration](../docs/arch/mmio.md#floating-point-acceleration)
and `runtime/include/vm_math.h`. The ordinary device mailbox stays physical;
its generic serialization requirements are unchanged.
