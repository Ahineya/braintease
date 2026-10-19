# Fine I/O deadlines and guest clocks

Implemented on October 17, 2026, for FUZIX on RVM and native Fuse-X. `select()`
and `usleep()` use absolute monotonic deadlines independently of the original
FUZIX decisecond timer queue. They no longer round short waits to 100 ms or add
an extra decisecond for tick phase alignment.

## Clock, deadlines and scheduling

These are separate mechanisms:

- The existing DEVICE host monotonic clock supplies a timestamp and resolution;
  both runtimes currently report 1 ms resolution.
- `ripple/deadline.c` maintains a sorted queue with one timer per process slot,
  up to 64. A clock update compares the head first, so it does not scan all
  processes when nothing is due. No active timers means a constant-time exit.
- Both platforms pass their existing host-clock sample to `ripple_select_clock`
  before doing coarse accounting. Expired timers wake their sleeping owners;
  ordinary readiness and signals can wake them earlier.
- The scheduler still determines when a ready process runs. RVM retains its
  instruction-based scheduling IRQ interval; native retains its checkpoints.
  No interrupt-frequency or ISA change is required. Idle device polling remains
  bounded by the existing 1 ms host idle request, which is skipped when polling
  has already made a process runnable.

The RVM kernel's runnable/event queues and elapsed-time scheduling budget are
described in [SCHEDULER.md](SCHEDULER.md). Its IRQ interval remains unchanged;
the normal slice is now based on elapsed time rather than counting IRQs alone.

This is a fine-resolution deadline mechanism, not a hard real-time guarantee.
Host scheduling, VM execution/JIT cost, disabled interrupts and guest scheduling
can delay a return. A requested timeout does not expire early because a coarse
counter was truncated. Sub-millisecond waits cross at least the next supported
clock quantum. This is still bounded idle polling, not host event subscription
or a fully tickless kernel.

The queue stores explicit high/low seconds and nanoseconds; it does not truncate
long waits to a wrapping millisecond counter. Arming adds the requested duration
with carry checks. An unrepresentable deadline returns `EOVERFLOW`. Readiness,
timeout, EINTR and fatal process exit remove the timer before reuse. A spurious
wake rechecks the original deadline instead of extending it. Clock-read failure
marks queued timed waiters for `EIO`; the queue core's failure behavior is tested
with a synthetic clock.

Registration, deadline arming, readiness checking and the sleep transition are
protected against interrupts. Expiry can be recorded while an owner is running
or stopped; it only wakes a process actually sleeping on its own timer record.
This avoids losing an expiry just before sleep and prevents a stale timer from
waking an unrelated operation. Timers need neither heap allocation nor process
structure/ABI changes.

Alarms, legacy `_pause`/`sleep`, tty VTIME, calendar advancement, CPU accounting
and existing tick-based counters keep their old units. The fine queue does not
consume or replace a process alarm.

## Userspace monotonic clock

`ripple/libc/clock.c` supplies the shared `clock_gettime` and `clock_getres`
wrappers. `CLOCK_MONOTONIC` now reports the fine clock rather than reconstructing
a timestamp from the legacy decisecond counter. `CLOCK_REALTIME` still uses the
guest calendar, with its existing one-second resolution and `stime` behavior.
`clock_getres(CLOCK_MONOTONIC, ...)` reports the runtime's actual resolution.
Invalid clock IDs return `EINVAL`; `clock_getres(valid_clock, NULL)` succeeds.
The existing public 32-bit `time_t` causes `EOVERFLOW` if monotonic seconds cannot
fit it; the internal deadline queue retains the full seconds value.

[`ripple/clock-abi.h`](ripple/clock-abi.h) adds selector 2 to syscall 27. Its output
is eight C16 cells: four unsigned 32-bit fields, in order: seconds low, seconds
high, nanoseconds, resolution in nanoseconds. Kernel copy-out validates the guest
buffer. No host structure layout or direct userspace MMIO access is used.
Selectors 0 (calendar) and 1 (raw decisecond counter) preserve their previous
payload and meaning; the original kernel implementation remains available under
`ripple_legacy_time` and handles those calls.

Existing guest binaries keep their embedded clock wrappers. Their select waits
benefit from the new kernel, but rebuild/reinstall them to obtain fine userspace
monotonic timestamps. Normal RVM/Fuse-X application builds pick up the new libc;
no source API changes are needed. New clock wrappers require the updated kernel.

## Validation

With the corresponding kernel and bootstrap apps built, from the repository root:

```sh
python3 ports/fuzix/tools/test_deadlines.py
python3 ports/fuzix/tools/test_deadlines.py --native
python3 ports/fuzix/tools/test_select.py
python3 ports/fuzix/tools/test_select.py --native
python3 ports/fuzix/tools/test_curses.py
python3 ports/fuzix/tools/test_curses.py --native
python3 ports/fuzix/tools/test_timers.py
```

`make -C ports/fuzix BUILD="$PWD/build/fuzix" test-deadlines` builds the RVM
prerequisites. The deadline probe is also part of both normal regression suites.
All tests use disposable disks; the select network fixture uses a local Ethernet
peer and needs no Internet access.

`finecheck.c` passes 369 parent-process assertions on each target, plus child
checks and host-side assertions. It exercises actual guest syscalls and tests
the same deadline queue core with synthetic timestamps: nanosecond carry,
32-bit seconds rollover, very long waits, equal deadlines, all 64 slots,
head/middle/tail cancellation, rearming, exact-boundary expiry, clock failure,
64-bit overflow and reuse. Guest checks cover the clock payload/canaries,
resolution and errors, 1/5/10/20/50 ms waits, no early expiry, unchanged timeout
buffers, signals, alarm preservation, spurious wakes, simultaneous sleepers,
fatal cleanup and a timeout while another process computes without syscalls.
Host checks cover idle CPU use and expiry during SIGSTOP/SIGCONT of the host.

One recorded run measured these averages over four waits per duration:

| Requested | Native Fuse-X | RVM JIT |
| --- | ---: | ---: |
| 1 ms | 1 ms | 6 ms |
| 5 ms | 5 ms | 6 ms |
| 10 ms | 10 ms | 20 ms |
| 20 ms | 20 ms | 21 ms |
| 50 ms | 50 ms | 52 ms |

A separate 50 ms `usleep` measured 51 ms native and 52 ms RVM. During a two-second
idle wait the processes consumed about 0.08 s and 0.18 s of host CPU respectively.
An already expired deadline resumed at the syscall return within roughly
21/25 ms of host continuation. These are local measurements under concurrent
development load, not latency guarantees. The baseline native kernel rounded a
requested 1 ms wait to approximately 200 ms on this same probe.

See the [select contract](../../docs/networking/select.md) for readiness,
signals, calendar isolation and networking regression coverage, and
[CURSES.md](CURSES.md) for terminal integration.
