# MMIO

Bank 0, words **0–31** are the MMIO header. TEXT40 VRAM starts at word **32**. Full device semantics, bitfields, and examples: [`rvm/mmio.md`](../../rvm/mmio.md). Constants: `rvm/src/constants.rs` and `runtime/include/mmio_constants.h`.

## Header

| Addr | Name | R/W | Role |
|------|------|-----|------|
| 0 | TTY_OUT | W | low 8 bits → stdout |
| 1 | TTY_STATUS | R | bit 0 ready |
| 2 | TTY_IN_POP | R | pop input byte |
| 3 | TTY_IN_STATUS | R | bit 0 has byte |
| 4 | RNG | R | next PRNG (advances) |
| 5 | RNG_SEED | R/W | low 16 bits of seed |
| 6 | DISP_MODE | R/W | 0=OFF 1=TTY 2=TEXT40 3=RGB565 4=RGB565_EXT |
| 7 | DISP_STATUS | R | ready / flush_done |
| 8 | DISP_CTL | R/W | enable / clear |
| 9 | DISP_FLUSH | W | present frame |
| 10–15 | Reserved | R/W | reads zero; writes ignored |
| 16 | DISP_RESOLUTION | R/W | RGB565 hi8=width lo8=height |
| 17–20 | STORE_* | | block storage (16-bit word addressed) |
| 21 | IRQ_STATUS | R/W | pending bits; **write ORs** |
| 22 | IRQ_ENABLE | R/W | enable mask |
| 23 | IRQ_BUSY | R/W | R: in-service; W nonzero = ACK |
| 24 | IRQ_VECTOR_BANK | R/W | handler PCB |
| 25 | IRQ_VECTOR_OFF | R/W | handler PC |
| 26 | IRQ_CAUSE | R | 0=none, else 1 + bit index |
| 27 | DEVICE_DESC_BANK | R/W | command descriptor bank |
| 28 | DEVICE_DESC_OFF | R/W | command descriptor cell offset |
| 29 | DEVICE_COMMAND | W | submit device command |
| 30 | DEVICE_STATUS | R | command result; 0=success, 0x8000=pending |
| 31 | DEVICE_VERSION | R | device ABI version (1) |

C helpers: `runtime/include/mmio.h` (`tty_*`, `rng_*`, `storage_*`, `irq_*`, TEXT40, keyboard).

## Versioned device commands

The five `DEVICE_*` registers are a mailbox for requests stored elsewhere in
guest RAM. A descriptor is a small request-and-reply record: the host reads its
inputs and writes its outputs at the guest address you supply.

For example, put `[1, 4, 0, 0]` in four cells at bank 2, offset 100. Write 2 to
bank-0 address 27, 100 to address 28, then 2 (`CAPS`) to address 29. On success,
address 30 reads zero and the record contains `[1, 4, capability_bits, 256]`.
Only writing address 29 submits a command. The pointer selects guest data RAM,
and the host validates its range; it is not a host process pointer.

See the [worked memory and C examples](../../rvm/mmio.md#worked-example-a-capabilities-request-in-memory)
and the [complete example using C helpers](../../rvm/mmio.md#complete-c-example-using-the-helpers).

`runtime/include/vm_devices.h` provides framebuffer presentation, a logical
simulation clock, ordered keyboard/pointer events, and input state. The descriptor starts with
`[version, length]`, followed by command-specific 16-bit cells. Lengths, offsets,
and framebuffer strides count **cells**, matching the compiler's 16-bit C byte.
The descriptor may cross a bank boundary. Set its bank/offset, write the command,
then read status. `NEXT_TICK` yields execution until the host grants the next tick;
it is complete before the next guest instruction. There is no polling loop.

| Command | ID | Descriptor cells | Operation |
|---------|----|------------------|-----------|
| PRESENT | 1 | 8 | snapshot an RGB565 framebuffer in mode 4 |
| CAPS | 2 | 4 | read capability bits and key count |
| NEXT_TICK | 3 | 8 | request one simulation tick and read logical time |
| GET_TIME | 4 | 8 | read logical time without advancing it |
| INPUT_READ | 5 | 10 | drain up to 64 typed keyboard/pointer events |
| INPUT_STATE | 6 | 28 | read keys, pointer, focus/capture and sequence; acknowledge overflow |
| INPUT_CONFIGURE | 7 | 4 | request absolute or relative pointer mode |

Mode 3 retains its bank-0 double-buffer layout, 8-bit dimensions, and flush
register. Mode 4 accepts 16-bit width/height/stride and a banked framebuffer pointer
through `PRESENT`; it does not reserve pixel storage in bank 0. Presentation and
time reads never advance the clock. See the [full wire contract](../../rvm/mmio.md#device-command-abi-version-1)
and [Doom platform guide](../doom-platform.md).

Hosts advertising `VM_CAP_INPUT_POLL` (0x0010) collect live keyboard/pointer input on
`INPUT_READ` and `INPUT_STATE`, independently of clock grants. Live events carry
the current logical tick, including zero; scheduled replay and stepped-controller
events remain tied to tick grants.

The ordered stream includes keys, motion, buttons, wheel, enter/leave, focus and
capture. Absolute positions count guest pixels; relative motion counts raw device
units. Wheels use signed 16.16 lines. Relative capture is asynchronous and releases
on focus loss. The [event and snapshot layouts](../../rvm/mmio.md#keyboard-and-pointer-events-and-state)
define flags, batching, overflow recovery and live/replay delivery. ABI version
remains 1; there are no alternate input layouts.

The [audio device](audio.md) adds PCM CONFIGURE/SUBMIT/STATE/CONTROL/RENDER
commands at 0x0200–0x0204 and OPL3 music bank/score/control/state commands at
0x0210–0x0213. Capabilities 0x0080 (PCM) and 0x0100 (music) are available in
RVM and the RCC native runtime; RBF reports audio unsupported. No new MMIO
addresses are used.

These are host-device extensions; instruction encoding and CPU semantics are
unchanged. The Rust VM, Brainfuck host adapter, and native runtime implement this
device-command ABI. The Brainfuck CPU continues to execute guest instructions
in Brainfuck.

The [networking contract](../networking/device.md) adds `NET_INFO` (0x0100),
`NET_TX` (0x0101), `NET_RX` (0x0102), and `NET_STATS` (0x0103) in the same mailbox,
advertised by capability 0x0020. It documents queues, one-octet-per-cell buffers,
the C helpers, host attachment and bidirectional ping tests. No additional MMIO
addresses are used.

The [bulk I/O contract](bulk-io.md) adds storage read/write and RAM/code copy/fill
commands at 0x0300–0x0303. RVM and RBF can transfer attached-disk contents directly
into guest RAM or instruction memory. Capabilities distinguish storage, RAM, and
code support; transfers share the legacy disk cache and use the same mailbox.

## Interrupts

IRQ ABI version 2 preserves RA/RAB and holds the interrupted PC/PCB and stack
state in CPU control registers. ACK acknowledges the active event while BUSY
continues to inhibit entry. IRET restores saved state and ends the handler
atomically. Pending events raised during a handler are retained.

CPU control registers, optional interrupt-stack switching, and the deterministic
instruction timer are defined in [stacks-and-interrupts.md](stacks-and-interrupts.md).
The device mailbox and all existing MMIO addresses are unchanged.

Storage COMMIT/COMMIT_ALL are durable host-file barriers in RVM. STORE_CTL bit 4
is a sticky host I/O error (write one to clear); see [storage semantics](../../rvm/mmio.md#durable-storage-commits-and-errors).
