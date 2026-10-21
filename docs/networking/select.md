# select() implementation contract

Implemented for Riplix and native Fuse-X on October 17, 2026. RVM's
`gterm` uses this interface for graphics and PTY waiting. It is also shared
groundwork for the [terminal browser](terminal-browser.md) and Nano-X; neither
application port is completed by supplying readiness.

## What select does

An open terminal, socket, pipe or file has a process-local integer descriptor.
`select()` waits until requested descriptors are ready for an operation, a
timeout expires or a signal interrupts the wait. It reports readiness; the
application then performs `read()`, `write()` or `accept()` itself.

A browser can ask: "Wake me when the keyboard has input, the server sends data,
or five seconds pass." While it waits, the scheduler can run other processes
or idle the host. This keeps the browser responsive during downloads without
an application loop repeatedly probing every descriptor.

The conventional userspace signature is:

```c
int select(int nfds, fd_set *readfds, fd_set *writefds,
           fd_set *exceptfds, struct timeval *timeout);
```

`nfds` is one greater than the highest descriptor to inspect, not a count of
set bits. Each non-null set is an input/output argument: on success it contains
only the requested descriptors that are ready. Applications rebuild the sets
before the next call. A null timeout waits indefinitely; a zero timeout checks
once without sleeping. `nfds == 0` with a finite timeout is a timed sleep.

Return a positive count of ready bits across the returned sets, zero on timeout,
or `-1` with `errno` on failure. A descriptor ready in two sets contributes two
bits to that count. Readability includes EOF; readiness does not guarantee a
whole application message can be transferred. Use nonblocking I/O and handle
partial progress and `EAGAIN` after wakeup when appropriate.

## Implementation

`CONFIG_SELECT` enables descriptor readiness independently of `CONFIG_LEVEL_2`
and preserves job control. Syscall 72 dispatches to
[`ripple/selectwait.c`](../../ports/fuzix/ripple/selectwait.c). The upstream
[`Kernel/select.c`](../../ports/fuzix/upstream/Kernel/select.c) supplies inode
waiter maps and wake helpers; its legacy single-word syscall is excluded on
Ripple. Both targets use the same guest wait core and socket bridge.

The public [`sys/select.h`](../../ports/fuzix/ripple/include/sys/select.h)
provides the conventional API. New `fd_set` values have eight 16-bit words
and capacity 128. RVM, Fuse-X and freestanding x86 accept `nfds` through 128
and return `EINVAL` above that limit. Descriptor bits at or above `nfds` are ignored.
`poll()` and `pselect()` remain unimplemented.

Wait registrations live in in-memory inode maps, separate from filesystem
metadata. Registration, readiness testing and entering sleep run under IRQ
protection. Device/queue transitions wake interested processes, which recheck
state; applications do not run a polling loop. Normal return clears all maps
and the syscall timer. Exit also clears a killed selector's registrations
before descriptors or process slots can be reused. A fast path avoids device
inode scans when there are no selectors.

[`select-abi.h`](../../ports/fuzix/ripple/select-abi.h) defines syscall 72's
versioned request: one version cell followed by three descriptor-set pointers
and a timeout pointer (nine C16 cells total). The first syscall argument is
`nfds`; the second points at this request. Version 1 keeps exactly two mask
cells and accepts at most 32 descriptors; version 2 uses eight mask cells and
accepts 128. The request itself keeps the same nine-cell shape. Each non-null
set is copied using its version's exact size; a non-null timeout points at two 32-bit fields matching the target
`timeval`. The kernel validates and copies these buffers before registering
waits, so invalid public pointers return `EFAULT` instead of being dereferenced
by the libc wrapper. Public timeout storage is never written.

The updated header maps source calls to `ripple_select_wide`; libc retains
the original `select` symbol with its two-word contract for legacy clients.
Low-descriptor calls from the new wrapper still use version 1, so they remain
usable on an older kernel. High-descriptor calls require the updated kernel.
Existing statically linked programs keep their original wrapper. Shared SDK
packages and their consumers follow the normal coordinated rebuild/install
policy. No guest-to-host networking ABI or device-driver change is involved.

Timeout seconds accept `0..0x7fffffff`, and microseconds `0..999999`; wrapped
negative assignments to the existing unsigned timeval fields are rejected.
Finite waits use absolute deadlines from the host monotonic device,
independently of calendar changes. A separate sorted deadline queue preserves
full high/low seconds and nanoseconds, avoiding the legacy 16-bit decisecond
timer and its 100 ms rounding/extra phase tick. Both runtimes currently expose
1 ms clock resolution. Existing clock-update opportunities expire due waits;
process scheduling and host/VM overhead can delay their return. Positive
sub-resolution waits cross the next clock quantum; spurious wakes retain the
original deadline. Select does not replace or consume process alarms. See
[fine deadlines and guest clocks](../../ports/fuzix/TIMERS.md) for the internal
queue, userspace clock ABI, measurements and tests.

Sockets expose `getsockopt(SOL_SOCKET, SO_ERROR)` for asynchronous connection
completion. A pending error is cleared only after successful validated output;
repeated queries return zero until another error occurs. Other socket options
return `EOPNOTSUPP`. Urgent data is unsupported and `exceptfds` stays empty.
TCP write readiness follows connection state and send-buffer capacity. UDP/raw
readiness observes shared transmit pressure and known ARP resolution state;
a first send may initiate resolution and return `EAGAIN`, so callers retain
normal nonblocking retry handling.

RVM `/dev/gfx` is readable when the owning process has a queued event, overflow
notice or close request; querying readiness does not dequeue it. Retrieve the
event with `GX_EVENT`. Synchronous drawing requests make the graphics descriptor
writable. A process without the lease is immediately ready for the operation's
permission/handle error, rather than sleeping indefinitely. This is the current
single-owner GX contract, not a multiwindow subscription API. Native Fuse-X
currently has no graphics device.

`gterm` rebuilds read sets for graphics and its PTY master, adds PTY write
interest only while keyboard replies are pending, and uses a finite timeout
for key-repeat/child checks. `GX_WAIT` remains available for older applications;
new consumers should use descriptor readiness. Unix-domain local IPC now
supplies service descriptors on both targets. Nano-X
still needs protocol adaptation and several-window ownership/lifecycle work
before a split desktop server.

## Interface and ABI decisions

1. Provide the conventional `select()` and `FD_ZERO`, `FD_SET`, `FD_CLR` and
   `FD_ISSET` API through `sys/select.h`. Reuse the existing `struct timeval`
   declaration in `sys/time.h`; do not introduce an incompatible duplicate.
2. Use a C16 `fd_set` with capacity 128, represented by eight 16-bit mask
   words. Keep `FD_SETSIZE` distinct from the active process limit. Both
   maintained kernels support all 128 process descriptors. Do not shift a 16-bit `int` by 16 or more.
3. Keep the public interface independent of the internal syscall representation.
   Syscall 72 uses the versioned C16 pointer request described above, replacing
   the old 16-descriptor payload and byte-offset assumptions. No host `fd_set`,
   `timeval`, pointer or structure layout crosses this boundary.
4. Validate `nfds`, selected descriptors, timeout fields and all user buffers
   before registering waits. Reject negative or over-limit `nfds` and invalid
   timeouts with `EINVAL`, selected closed descriptors with `EBADF`, and invalid
   user addresses with `EFAULT`. Ignore bits at or above `nfds`.
5. Return `EINTR` for an interrupting caught signal. Keep the caller's timeout
   unchanged; callers should reconstruct sets after every call and must not
   depend on set contents after an error. Do not consume application alarms
   to implement a select timeout.
6. Supply readiness and bounded waiting through the guest kernel on both
   platforms. The host transports Ethernet and input events; it must not
   substitute host socket selection for the guest socket state machine.

Code importing an existing browser must include the target header and compile
against the target ABI. A host-configured descriptor-set layout is not usable.
Keep `poll()` and `pselect()` outside the first milestone; applications selecting
those APIs need a deliberate configuration choice or a later implementation.

## Readiness semantics

Readiness means the requested operation can complete immediately with data,
EOF, progress or an applicable error, rather than needing to wait for a future
event. Only report descriptors from the caller's requested sets.

| Descriptor | Read readiness | Write readiness |
| --- | --- | --- |
| Regular file | Immediate, including at EOF | Immediate; the subsequent operation reports access/storage errors |
| Directory | Immediate where reading is supported | Follow the directory operation's immediate-error behavior |
| Terminal/PTY | Input available according to the endpoint's termios rules, or hangup/EOF | Output queue can accept data, or the operation will immediately report an error |
| Pipe | Buffered data, or EOF after the last writer closes | Space for progress, or immediate broken-pipe failure after the last reader closes |
| Connected TCP socket | Received data, peer half-close/EOF, or a pending error | Send space in a usable connection, or a pending failure; do not mark every socket always writable |
| Listening socket | An accepted connection is queued, or an immediate relevant error | No readiness merely because the endpoint is a socket |
| UDP socket | A datagram is queued, including a zero-length datagram, or a pending error | The guest send path can accept a datagram or immediately report its error |

Handle asynchronous connect completion if exposed by the socket API: completion
or failure wakes the writer, and the application needs a way to distinguish
success from failure. Audit `getsockopt(SO_ERROR)` and its read/clear behavior
with the browser; do not mistake write readiness for successful connection.

`exceptfds` describes exceptional conditions such as supported urgent data.
Ordinary EOF, hangup and connection errors must wake the relevant read/write
waiters. Do not invent an urgent-data feature or use `exceptfds` as a catch-all
error set. Document unsupported socket exceptional conditions.

Readiness checks must not consume bytes, accept a connection, clear an error,
change descriptor flags or change terminal modes. Descriptor duplicates refer
to the same underlying state, but each requested descriptor appears separately
in the returned sets.

## Waiting, wakeups and time

Use the existing scheduler and device/network polling paths. The critical
sequence is:

1. Copy in and validate the request.
2. Under the kernel's interrupt/scheduler protection, register interest and
   test the relevant state without a gap in which an event can be lost.
3. Return immediately if ready, interrupted or doing a zero-timeout poll;
   otherwise atomically enter the process wait state.
4. Wake on a relevant state transition, signal or deadline. Recheck readiness
   after every wake; a wakeup is not itself proof of readiness.
5. Remove every registration and timer on every return path, including errors,
   hangup and signal interruption.

Registration and state testing may use a different ordering if the same
no-lost-wakeup property is demonstrated. A polling loop that keeps the process
runnable does not satisfy this contract. Native continuations must suspend
the waiting process while unrelated sessions and host idle processing proceed.

Wake sources include terminal input, PTY queue changes, pipe reads/writes and
last-end close, TCP RX and TX-space changes, connection completion, accept
queue changes, EOF, reset, network loss and relevant UDP events. Follow the
existing descriptor-sharing and last-close rules across `dup()` and `fork()`.
Cleanup must leave no waiter attached to a reused process, inode or endpoint.

Use monotonic elapsed time, not the guest calendar. Validate microseconds
against `0 <= tv_usec < 1000000`; audit the existing unsigned timeval field
types so invalid negative assignments do not turn into accepted huge waits.
Preserve the distinction between a zero timeout and an indefinite wait.
Represent large timeouts as absolute monotonic deadlines with checked carries;
avoid overflow, early expiry, and deadline extension after spurious wakeups.
Keep fine I/O expiry independent of decisecond accounting and alarm timers.

## Integration map

| Layer | Integrated support |
| --- | --- |
| Public libc | C16 descriptor sets/macros and a thin syscall wrapper; kernel validates buffers and timeout, and counts ready bits |
| Syscall ABI | Shared payload layout, syscall 72 wrapper/dispatch, bank-aware copy-in/out and pointer validation |
| Kernel wait core | Full descriptor range, registrations, timers, signals and balanced cleanup |
| Files/pipes | Immediate file results, pipe queue/EOF/broken-pipe transitions |
| TTY/PTY | Queue readiness, terminal mode semantics, hangup and wakeup integration |
| Sockets | Readiness queries and wake notifications from guest network state changes |
| Builds | Required sources/feature guards in RVM and native; no accidental dependency on all level-2 services |

Only the bundled inode wait-map helpers are reused by Ripple. Its legacy
syscall payload, timeout logic and device probing are replaced by the shared
port implementation.

## Acceptance criteria

Use deterministic local fixtures before live internet tests. Required checks:

- **API:** empty/null sets, finite and zero timeouts, indefinite wait interrupted
  by an event, `nfds == 0`, invalid descriptors/addresses/timeouts, ignored bits
  above `nfds`, correct return counts, and descriptors 15, 16 and 31 on RVM.
  Verify native's explicit limit behavior and all four set macros.
- **Files/pipes:** regular file EOF, pipe data, full-to-writable transition,
  EOF and last-reader closure. Cover duplicates and forked endpoints.
- **Terminal:** keyboard arrival, canonical versus noncanonical input,
  VMIN/VTIME interactions, PTY output space, hangup and signal interruption.
- **Network:** TCP data and EOF, half-close, partial writes/backpressure,
  failed connection and reset, listener accept, UDP including empty datagrams,
  and link loss. Check `SO_ERROR` if asynchronous connect uses it.
- **Races/lifecycle:** event arriving at the check/sleep boundary, spurious
  wakeups, repeated timeout/EINTR/reconnect cycles, waiter cleanup, endpoint
  reuse and independent waiters on a shared underlying descriptor.
- **Time/scheduling:** no early timeout from integer truncation, bounded
  rounding error, long-wait conversion, calendar adjustment during a wait,
  low idle CPU use and progress in another terminal while one process waits.
- **Targets:** run kernel/libc behavior on RVM and native separately; compilation
  or a host-only readiness test is insufficient.

The final application-level check is a browser waiting for keyboard and HTTP
input together: cancelling a slow response must work without disrupting the
Telnet session or other processes. See the
[browser acceptance gates](terminal-browser.md#implementation-order-and-acceptance-gates).

## Reproducible verification

```sh
make -C ports/fuzix BUILD="$PWD/build/fuzix" images
python3 ports/fuzix/tools/test_select.py --build build/fuzix
make -C fuse-x all
python3 ports/fuzix/tools/test_select.py --native
python3 ports/fuzix/tools/test_deadlines.py
python3 ports/fuzix/tools/test_deadlines.py --native
FUZIX_TEST_BUILD="$PWD/build/fuzix" cargo test --release -p rvm --test fuzix_graphics -- --ignored --nocapture
```

The shared `selectcheck.c` probe and independent local Ethernet fixture cover
API validation and mask counts, high descriptors, bank-spanning user buffers,
regular-file EOF, pipe data/backpressure/last-close, duplicate/forked endpoints,
independent waiters, finite/zero/indefinite waits, long timeouts interrupted by
signals, alarm preservation, calendar changes, spurious wakes and killed-waiter
reuse. PTY coverage includes canonical EOF, VMIN/VTIME modes, both queue
directions and hangup. Live input and a two-second low-CPU wait are checked on
both targets. Network coverage includes asynchronous connect success/refusal,
SO_ERROR read/clear, TCP streams/EOF, UDP including empty datagrams, listener
accept, reset and administratively dropping the interface. These tests use
private disks and a local Ethernet peer; they need no internet access.

The existing RVM framebuffer test injects real device events into a JIT guest
and checks exact terminal pixels, scrolling, key repeat, focus loss and close.
The graphics probe checks graphics-descriptor readiness alongside upload,
ownership and PTY regressions. Job-control and ordinary filesystem integration
checks remain separate regression suites. The acceptance criteria above are
the contract; these probes are not an exhaustive proof of every possible
interleaving or a completed browser/Nano-X integration test.

Unix-domain stream sockets and `socketpair()` now share this readiness contract
on both targets, independently of Ethernet. See [local IPC and expanded
networking capacity](local-ipc.md) for queues, ownership and limits.

The file-capacity regression in `ports/fuzix/tools/fdcheck.c` additionally
checks all 128 handles, both select versions and legacy copy canaries,
descriptors 31/32/63/64/95/96/126/127 across fork/exec, high pipe/socket
readiness, blocked high-descriptor wakeups, 20 concurrent popen streams,
stream flushing/errors/exit, and repeated 1,024-entry global exhaustion and
recovery. The larger open-file and select-registration tables allocate from
supervisor memory at boot; they do not consume the fixed kernel-global bank.
`test_file_limits.py` and `test_file_limits_x86.py` run on disposable roots.

For a private bounded Fuse-X build (from the repository root):

```sh
make -C fuse-x BUILD="$PWD/build/fd-limits/kernel" FUSE_X_HEAP_PROFILE=bounded -j4 "$PWD/build/fd-limits/kernel/fuse-x"
FUSE_X_HEAP_PROFILE=bounded FUSE_X_SHARED_LIBRARIES=0 python3 fuse-x/apps.py sh echo cat --build build/fd-limits/kernel
python3 fuse-x/cc.py ports/fuzix/tools/fdcheck.c --static --heap-profile bounded --build build/fd-limits/sdk -o build/fd-limits/fdcheck.fxe
python3 ports/fuzix/tools/test_file_limits.py --kernel build/fd-limits/kernel/fuse-x --build build/fd-limits/kernel --program build/fd-limits/fdcheck.fxe
make -C ports/fuzix BUILD="$PWD/build/fd-limits/rvm" -j4 test-file-limits
```

Using a private matching seed avoids borrowing an application/shared-library
pair that another workstream may rebuild during the test. The x86 harness
accepts a separately built `--program` and `--output` with normal boot assets;
`RCC_TEST_ACCEL=kvm` uses hardware acceleration on Linux.
