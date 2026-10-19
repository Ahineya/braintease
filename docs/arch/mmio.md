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
| 10 | MATH_STACK | R/W | RVM: read ABI 1; write packet offset in current SB |
| 11–15 | Reserved | R/W | reads zero; writes ignored |
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
| HOST_MONOTONIC | 8 | 10 | read host elapsed time independently of simulation ticks |
| HOST_REALTIME | 9 | 10 | read host Unix UTC calendar time |
| HOST_IDLE | 10 | 4 | yield or request a bounded host wait of at most 10 ms |
| HOST_TERMINAL | 11 | 4 | read live host terminal rows and columns |
| HOST_RANDOM | 12 | 6 | fill RAM cells with host cryptographic random octets |
| CRYPTO | 13 | 6 | stateless ECC and public RSA calculations |
| TTY_WRITE | 14 | 8 | synchronously print up to 4096 raw octets from RAM |
| MATH | 15 | 16 | IEEE binary32/binary64 arithmetic, conversions and binary64 libm |

`TTY_WRITE` submits `[1, 8, bank, offset, count, 0, 0, 0]`. The host copies the
low eight bits of each source cell to stdout and flushes before returning.
Count is 0–4096; zero flushes without printing (the pointer must still identify
ordinary RAM). Source and descriptor may cross banks but cannot overlap or
address MMIO. Reserved cells must be zero. It grants no tick and raises no IRQ.
Cell 6 reports octets accepted. Invalid requests have no output or memory
effects; an `IO` error can report partial progress in cell 6, which must not be
replayed. It is a synchronous DMA-style transfer, without a worker or retained
guest pointer. No additional MMIO registers or capability bits are needed;
`UNKNOWN_COMMAND`/`UNSUPPORTED` permit a scalar fallback.

`vm_tty_write()` exposes status and progress. Standalone `tty_write_bytes()`
uses it with a legacy fallback, and `puts`/`printf` batch their text through it.
Scalar `putchar` keeps the legacy host newline handling. Riplix/Fuse-X libc keeps
using ordinary `write`: the shared physical-console path stages 256 user cells
in kernel RAM before DMA, preserving foreground checks, STOP/DISCARD and output
processing. Characters requiring newline conversion use the existing scalar
path. PTY/Telnet queues and redirected files keep their existing device paths.
The native runtime flushes pending scalar output before DMA to preserve order.
Callers must serialize the mailbox against interrupts; MMU user pointers must
never be submitted as physical DMA pointers.

`VM_CAP_HOST_RANDOM` (0x8000) advertises host OS entropy. Submit
`[1, 6, bank, offset, count, 0]`, with count 1–256. Each output cell contains
one octet; invalid ranges, descriptor overlap and OS failures leave RAM
unchanged. This is independent of the seeded `RNG` registers. See the
[entropy contract and guest devices](entropy.md).

`CRYPTO` uses its own query operation for discovery; the original CAPS bits are full. See the [crypto coprocessor contract](crypto.md).

Capability `VM_CAP_HOST_CLOCK` (0x1000) advertises both host clocks in RVM,
RBF and native C. Both return `[1, 10, seconds (four cells), nanoseconds
(two cells), resolution_ns (two cells)]`, least-significant cell first.
Resolution is 1 ms; nanoseconds are in 0..999,999,999. These synchronous reads
do not grant simulation ticks or generate IRQs. `HOST_MONOTONIC` starts at its
first read after reset, includes time while the VM is paused, and cannot move
backward with calendar changes. Checkpoint restore resumes saved elapsed time,
excluding time spent saved. `HOST_REALTIME` reads the host calendar on every
request and may jump. `VmHostTime` represents seconds as two 32-bit C fields.
Host clocks are deliberately nondeterministic; deterministic games/replays
should keep using `GET_TIME`/`NEXT_TICK`.

`VM_CAP_HOST_IDLE` (0x2000) advertises `HOST_IDLE`. Submit `[1, 4, ms, 0]`,
where `ms` is 0..10: zero yields, positive values request a host sleep. The
descriptor stays unchanged. Larger durations or nonzero reserved cells fail
before waiting. It grants no simulation tick, generates no IRQ, consumes no
input and starts no network. A host scheduler can oversleep; callers must
reread monotonic time. This is a bounded polling fallback, not an event
subscription. See the [idle contract and reactor example](host-idle.md).

`VM_CAP_HOST_TERMINAL` (0x4000) advertises the terminal-size query on RVM and
native hosts. Submit `[1, 4, 0, 0]`; success returns `[1, 4, rows, columns]`.
Both dimensions must be nonzero. Nonzero input payload cells are invalid;
without an available host terminal, the command returns `UNSUPPORTED` and
leaves the descriptor unchanged. It consumes no input, advances no simulation
tick and raises no device IRQ. Like the host clock, it samples live external
state rather than deterministic replay state. Existing hosts can report an
unknown command; clients retain a default or cached size.

Riplix and Fuse-X use this query for physical-console `TIOCGWINSZ`. A detected
change updates the console and sends guest `SIGWINCH`; PTYs keep client-set
dimensions. Fuse-X's attached console client forwards initial and changed
dimensions to the broker's kernel PTY through a separate control connection.

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
capture, plus opt-in guest window-close requests. Absolute positions count guest pixels; relative motion counts raw device
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
`MEMORY_INFO` (0x0304) reports RAM and code capacities without allocating memory.
It lets an OS size its code pool to the host's configured or exported capacity.

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

## Floating-point acceleration

`runtime/include/vm_math.h` defines the shared scalar contract. The request is
16 cells: `[1, 16, operation, reserved=0, A (4 cells), B (4 cells), result (4 cells)]`.
Multiword fields are least-significant cell first. Floating operands/results
are raw IEEE bits; integer operands/results are scalar values. Single precision
occupies the low 32 bits. Unused B must be zero. Query operation zero with A=B=0
returns 7: binary32 arithmetic, binary64 arithmetic, and binary64 math. CAPS has
no free bits, so discover this service with QUERY and check its status.

The two arithmetic ranges, 0x101–0x113 and 0x201–0x213, implement add/subtract,
multiply/divide, conversions from/to unsigned/signed 32/64-bit integers,
conversion from the other floating width, and six comparisons, in that order.
The `UI`/`SI` suffixes refer to 32-bit values (C16 `unsigned long`/`long`).
Integer conversion saturates, truncates toward zero, and maps NaN to zero.
Arithmetic uses round-to-nearest, ties-to-even, gradual underflow and signed
zeros; NaN results are canonical quiet NaNs. Comparisons retain IEEE unordered
semantics. Transcendental results use host libm and can differ in their last
bits between hosts. The guest does not expose host errno or floating exception
flags, or a changeable rounding environment. `rint` uses ties-to-even.

Operations 0x301–0x321 cover sqrt, floor, ceil, trunc, round, sin/cos/tan,
asin/acos/atan/atan2, hyperbolic and inverse hyperbolic functions, exp/expm1,
log/log1p/log2/log10, pow, cbrt, hypot, fabs, fmod, copysign, fmin/fmax and rint.
The header is authoritative for individual numbers. Invalid mailbox requests
leave RAM unchanged. Calls do not advance logical time, raise device IRQs,
retain pointers or allocate guest buffers.

### Atomic RVM stack submission

The ordinary mailbox uses physical pointers and requires serialization. For
frequent pure calculations, RVM additionally accepts one STORE to bank 0,
address 10. Its value is the packet offset in the **current SB**. The CPU reads
and validates the entire packet against the current data map and read/write
permissions, computes the result, then writes its result and the completion
status to packet cell 3. On a descriptor error only cell 3 changes. Translation
or COW faults change no packet cells and retry that one STORE after the kernel
resolves the fault. Packets may cross pages/banks. No shared mailbox registers
are touched, so an IRQ or process switch between packet construction and
submission cannot substitute another process's request.

This is a narrowly permitted user-mode operation; other physical MMIO remains
privileged. User code must first check bit 2 (`4`) of the readable
`MMU_FEATURES` CSR 28. Older RVMs do not permit user access to address 10.
Unprotected code can probe address 10 directly: ABI 1 means available, zero
means use the ordinary mailbox. The C helper also verifies that the packet's
bank equals current SB; otherwise it uses the fallback path.

### Runtime and libc integration

Standalone software float helpers try the atomic stack path, then the serialized
physical mailbox, and retain integer-only fallbacks. Riplix libc uses the atomic
path and falls back to syscall 27 selector `0x4D41`: the kernel validates and
stages the 16-cell user packet, submits it with IRQs disabled, then copies the
successful result back. The fallback preserves errno. Native Fuse-X already
uses hardware arithmetic and now exposes the same math operations through the
pure scalar `__rcc_host_math` service, including application-package bindings.

The floating SDK supplies addressable C double functions for the QuickJS Math
surface. Cheap fabs/copysign/floor/ceil/trunc stay local. Use `float-sdk` with
Riplix or `--float-format` with `fuse-x/cc.py`. Expanded libm requires an updated
host; an unavailable operation returns a quiet NaN and guest `ENOSYS`. This is
separate from the arithmetic helpers' software fallback. `RCC_SOFTWARE_FLOAT`
disables automatic arithmetic acceleration for reference builds.

Validation: `python3 runtime/tests/float/guest.py` boots both real kernels on
disposable disks, checks math function pointers, IEEE edge cases, concurrent
forked processes and the kernel staging fallback, and compares warm arithmetic
with explicitly software-only helpers. `python3 runtime/tests/float/standalone.py`
checks the direct RVM path. Both harnesses snapshot their compiler and save logs/results under `build/float-accel`.
