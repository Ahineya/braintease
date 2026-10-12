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
| 10–15 | KEY_* | R | TEXT40/RGB565 keys |
| 16 | DISP_RESOLUTION | R/W | RGB565 hi8=width lo8=height |
| 17–20 | STORE_* | | block storage (byte addressed) |
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

`runtime/include/vm_devices.h` provides framebuffer presentation, a logical
simulation clock, keyboard events, and held-key state. The descriptor starts with
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
| INPUT_POP | 5 | 8 | pop one keyboard transition |
| INPUT_STATE | 6 | 19 | read 256 held-key bits and clear sticky queue overflow |

Mode 3 retains its bank-0 double-buffer layout, 8-bit dimensions, and flush
register. Mode 4 accepts 16-bit width/height/stride and a banked framebuffer pointer
through `PRESENT`; it does not reserve pixel storage in bank 0. Presentation and
time reads never advance the clock. See the [full wire contract](../../rvm/mmio.md#device-command-abi-version-1)
and [Doom platform guide](../doom-platform.md).

These are host-device extensions; instruction encoding and CPU semantics are
unchanged. The Rust VM implements them. The Brainfuck VM needs the corresponding
host adapter before a guest using this ABI can run there; this is not an existing
Brainfuck capability.

## Interrupts

Unpacked registers (no packed cause field). At the **start** of each instruction, if `BUSY==0` and `(STATUS & ENABLE)!=0`, the VM takes the **lowest set bit**, sets `BUSY`, sets `CAUSE = 1 + bit`, saves PC/PCB into RA/RAB (instruction not yet executed), and jumps to the vector. No nesting while busy. Software source: `IRQ_SW` (bit 0).

ACK: write nonzero to `IRQ_BUSY` — clears busy, cause, and the STATUS bit for the in-service cause. Write 0 is ignored.

Do not enable a bit that is already pending unless a handler is installed.

Handler return: `RET` (uses RAB) or `JALR R0, RAB, RA`. ACK immediately before return so another pending IRQ cannot steal RA/RAB.
