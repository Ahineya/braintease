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
| 10      | `HDR_KEY_UP`          | R   | Arrow up key state (bit 0: 1=pressed, 0=released)     |
| 11      | `HDR_KEY_DOWN`        | R   | Arrow down key state (bit 0: 1=pressed, 0=released)   |
| 12      | `HDR_KEY_LEFT`        | R   | Arrow left key state (bit 0: 1=pressed, 0=released)   |
| 13      | `HDR_KEY_RIGHT`       | R   | Arrow right key state (bit 0: 1=pressed, 0=released)  |
| 14      | `HDR_KEY_Z`           | R   | Z key state (bit 0: 1=pressed, 0=released)            |
| 15      | `HDR_KEY_X`           | R   | X key state (bit 0: 1=pressed, 0=released)            |
| 16      | `HDR_DISP_RESOLUTION` | R/W | Display resolution for RGB565 (hi8=width, lo8=height) |
| 17      | `HDR_STORE_BLOCK`     | W   | Select current storage block (0-65535)                |
| 18      | `HDR_STORE_ADDR`      | W   | Select byte address within block (0-65535)            |
| 19      | `HDR_STORE_DATA`      | R/W | Data register: read/write byte at (block, addr)       |
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
- Selects byte address within current block (0-65535)
- Auto-increments after each HDR_STORE_DATA access
- Wraps to 0 after reaching 65535

**Storage Data (HDR_STORE_DATA)**
- Read/Write register at address 19
- Read: Returns byte at (block, addr) in low 8 bits (high 8 bits are 0)
- Write: Updates byte at (block, addr) using low 8 bits of value (high 8 bits ignored)
- Auto-increments HDR_STORE_ADDR after each operation

**Storage Control (HDR_STORE_CTL)**
- Read/Write register at address 20
- Control bits:
  - Bit 0 (BUSY): Read-only, 1 if VM is processing operation
  - Bit 1 (DIRTY): Read/Write, 1 if current block has uncommitted writes
  - Bit 2 (COMMIT): Write-only, writing 1 commits current block
  - Bit 3 (COMMIT_ALL): Write-only, writing 1 commits all dirty blocks
  - Bits 15-4: Reserved (read as 0)

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

### Keyboard Input (TEXT40 Mode Only)

**Overview**
- Keyboard input flags are only active when display mode is set to TEXT40
- Keys are polled when reading keyboard MMIO addresses
- Flags indicate momentary key state (1=key event detected, 0=no event)
- State is cleared before each poll, so keys must be held for continuous input

**Arrow Keys (HDR_KEY_UP/DOWN/LEFT/RIGHT)**
- Read-only registers at addresses 10-13
- Bit 0 indicates key state
- Used for navigation in games

**Action Keys (HDR_KEY_Z/X)**
- Read-only registers at addresses 14-15
- Bit 0 indicates key state
- Common game action buttons (e.g., jump, shoot)

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
| INPUT_POP | 5 | 8 | version, length, available (out), key (out), down (out), tick low (out), tick high (out), overflow (out) |
| INPUT_STATE | 6 | 19 | version, length, overflow (out), 16 held-key bitmap cells (out) |

#### Worked example: a capabilities request in memory

Assume bank 2, offsets 100–103 are available guest RAM. First write this record:

| Guest location | Before submission | Meaning | After successful `CAPS` on current hosts |
|----------------|-------------------|---------|-----------------------------------------|
| `(2, 100)` | 1 | descriptor version | 1 |
| `(2, 101)` | 4 | descriptor length, including these first two cells | 4 |
| `(2, 102)` | 0 | capability bits, output | 63 |
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
future hosts may return additional bits rather than exactly 63.

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
| 0x8000 | pending host tick grant |

`CAPS` returns bits 0–5 set (63): extended RGB565, logical clock, input events,
held-key state, independent live-input polling (`VM_CAP_INPUT_POLL`, 0x0010),
and Ethernet networking (`VM_CAP_NETWORK`, 0x0020).
Older ABI-1 hosts may lack bit 4 and collect live input only at tick grants.
Older hosts may also lack networking. Key count is 256. Unknown future capability bits should be
ignored. Error statuses must be checked before consuming output fields.

Networking uses additional command IDs in this same mailbox: `NET_INFO` 0x0100
(18 cells), `NET_TX` 0x0101 (8), `NET_RX` 0x0102 (9), and `NET_STATS` 0x0103 (27).
See the [network wire formats and examples](../docs/mmio.networking.md#commands-and-wire-format).
It needs no separate MMIO addresses. Packet transfers copy one eight-bit octet
per guest cell and do not advance time, consume keyboard events or publish frames.

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

#### Keyboard events and state

Keys use USB HID keyboard usage IDs in the range 0–255, rather than host library
key codes or ASCII. Common IDs are A–Z = 4–29, 1–9 = 30–38, 0 = 39, Enter = 40,
Escape = 41, Space = 44, right/left/down/up = 79/80/81/82, and left/right
Ctrl/Shift/Alt/GUI = 224–231. `runtime/include/vm_devices.h` defines named keys.

`INPUT_POP` returns one transition: `available=1`, key ID, `down=1` for press or
0 for release, and the logical tick at which it was recorded. An empty queue
returns `available=0`. Use `available` before reading the event's other fields.

With `VM_CAP_INPUT_POLL`, both input commands collect pending live keyboard
transitions without advancing the clock or requiring `NEXT_TICK`. Live events
carry the current logical tick when collected, including tick zero before the
first grant; this is an observation timestamp, not a wall-clock timestamp.
Clock reads, tick grants and presentations do not collect live window input.
Scripted input and explicit stepped-controller events retain their scheduled
tick-grant delivery, so polling cannot release future replay events.

Held keys persist until release; host auto-repeat must not create extra presses.

`INPUT_STATE` returns a bitmap: key `k` is bit `(k & 15)` of cell `(k >> 4)`.
Queue overflow is sticky and reported by both input commands. Reading state
returns and clears the overflow flag, so a guest that missed transitions can
resynchronize its held-key state. State is maintained even if events overflow.
When that flag was set, `INPUT_STATE` also discards the remaining queued events
because the returned bitmap supersedes them. With no overflow it leaves the
event queue intact.
The queue holds 256 transitions. A tick grant that overflows it reports status 6;
automated runs also fail on observed overflow rather than silently accepting an
incomplete replay.

The six legacy key registers remain available to existing guests. Extended
input supports Doom's additional controls without adding per-key MMIO addresses.

#### C interface and backend compatibility

Include `<vm_devices.h>` and link the runtime. Its wrappers construct descriptors
on the stack, extract the compiler's packed bank/offset pointer representation,
submit commands, check status, and copy successful outputs into typed structs.
Use `vm_devices_version()` for discovery, `vm_devices_caps()` for capabilities,
`vm_present_rgb565()` for output, `vm_next_tick()` / `vm_get_time()` for time,
and `vm_input_pop()` / `vm_input_state()` for keyboard input.

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
        if (vm_input_pop(&event) != VM_DEVICE_OK) return 6;
        if (event.overflow) {
            /* Recover the authoritative bitmap and discard stale events. */
            if (vm_input_state(&keys) != VM_DEVICE_OK) return 7;
            printf("resync up=%d\n", vm_key_is_down(&keys, VM_KEY_UP));
            break;
        }
        if (!event.available) break;
        printf("key=%u down=%u tick=%lu\n", event.key, event.down, event.tick);
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
`vm_input_pop()` again afterwards to consume them. An ordinary `INPUT_STATE`
snapshot leaves queued events intact; after overflow it clears stale events and
the overflow flag. An application maintaining controls must replace its own
held-key state from that recovery snapshot.

### MMIO Read Handling

The VM routes memory reads in this order:
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

### Keyboard Input
```asm
; Check if up arrow is pressed
LI    T0, 0        ; Bank 0
LI    T1, 10       ; HDR_KEY_UP
LOAD  T2, T0, T1
ANDI  T2, T2, 1
BEQ   T2, R0, not_pressed

; Handle up arrow press
; ... game logic ...

not_pressed:
```

### Storage Operations
```asm
; Write data to block 42, starting at byte 0
LI    A0, 42
LI    T0, 0        ; Bank 0
LI    T1, 17       ; HDR_STORE_BLOCK
STORE A0, T0, T1

LI    A0, 0
LI    T1, 18       ; HDR_STORE_ADDR
STORE A0, T0, T1

; Write "Hello" (one byte at a time)
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
LOAD  A0, T0, T1   ; Read first byte ('H')
LOAD  A1, T0, T1   ; Read second byte ('e') (auto-increment)
LOAD  A2, T0, T1   ; Read third byte ('l') (auto-increment)
LOAD  A3, T0, T1   ; Read fourth byte ('l') (auto-increment)
LOAD  X0, T0, T1   ; Read fifth byte ('o') (auto-increment)
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
pub const HDR_KEY_UP: usize        = 10;
pub const HDR_KEY_DOWN: usize      = 11;
pub const HDR_KEY_LEFT: usize      = 12;
pub const HDR_KEY_RIGHT: usize     = 13;
pub const HDR_KEY_Z: usize         = 14;
pub const HDR_KEY_X: usize         = 15;
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
