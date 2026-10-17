# Bounded host idle

`HOST_IDLE` is device ABI 1 command **10**, advertised by capability **0x2000**
(`VM_CAP_HOST_IDLE`). It shares the existing DEVICE_* mailbox and requires no
new instruction or MMIO address. RVM, the RBF host adapter and the RCC native
runtime use the same implementation.

## Descriptor and completion

| Cell | Input |
|---|---|
| 0 | ABI version, 1 |
| 1 | Descriptor length, 4 cells |
| 2 | Requested milliseconds, 0..10 |
| 3 | Reserved, must be zero |

Zero requests a host scheduler yield. Positive durations request a host sleep;
the guest resumes after the synchronous command returns. The descriptor is
never modified, including on success. Version, length and bounds use the normal
device errors. A duration above 10 or a nonzero reserved field returns
BAD_DESCRIPTOR before waiting. A pending NEXT_TICK retains ownership of the
mailbox: submitting HOST_IDLE then leaves that tick pending.

No logical simulation tick is granted. No guest instruction timer is advanced,
no IRQ is generated, no input is consumed and no NIC is initialized. Other host
services can continue on their existing threads, but this command does not
subscribe the guest to an event or return a wake reason. It is not a substitute
for guest protocol polling or window/input servicing.

Ten milliseconds bounds the **requested** sleep, not host scheduler latency.
The OS may resume the process late. Always sample HOST_MONOTONIC after waiting;
never advance a software clock by assuming the requested delay actually elapsed.
As with all DEVICE_* commands, guest interrupt handlers and other users must
serialize the mailbox.

The instruction set remains compatible with the Brainfuck CPU. A host without
this device extension returns UNKNOWN_COMMAND/UNSUPPORTED; guests must handle
that explicitly. They must not silently spin or use NEXT_TICK as a real clock.

## Guest C

```c
#include <vm_devices.h>

VmHostTime sample;
if (vm_host_idle(5) == VM_DEVICE_OK &&
    vm_host_monotonic(&sample) == VM_DEVICE_OK) {
    /* Process events and compare actual time with your deadlines. */
}
```

`vm_host_idle` also validates its duration before submitting the command.

## RFL

`std/time` reads full-width host samples and supplies wrapping 32-bit monotonic
milliseconds. `std/host::host_reactor()` returns a reactor for timers;
`host_reactor_with(service, quantum)` adds a bounded service callback. The
callback runs before and after waiting and can return its next maintenance
delay, request an immediate turn, or stop the reactor. Networking remains a
separate library integration.

Run `target/release/rfl run rfl/examples/real-timers.rfl`, optionally with
`--target native`. The example runs two real timers and a yielding task in one
task scope, without advancing simulation time. See the
[RFL reactor contract](../networking/rfl.md#real-time-reactors).

## Validation

`rvm/tests/host_idle.rs` covers descriptor errors, bank crossings, unchanged
memory/input/ticks, pending mailbox ownership, a strided BF memory adapter and
actual interpreter/JIT execution. `rfl/tests/reactor.rs` checks deadline policy
with injected clocks, service events and failures on native and RVM, plus a
real host clock/idle/timer case. `c-test/tests/runtime/test_vm_host_idle.c`
exercises the archived guest C helpers and their normal inline MMIO path.
Timing checks do not impose an upper wall-time
limit on host scheduling. No full RBF networking run is required.
