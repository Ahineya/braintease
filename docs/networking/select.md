# select() implementation contract

Planned FUZIX/Ripple and native Fuse-X prerequisite, recorded October 17, 2026.
Nothing in this page marks the API implemented. The first consumer is the
[terminal browser port](terminal-browser.md), but the interface should also
serve terminal tools, chat clients and network servers.

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

## Current code and missing integration

- [Common configuration](../../ports/fuzix/ripple/config-common.h) enables
  `CONFIG_JOB_CONTROL`, not `CONFIG_LEVEL_2`.
- [Kernel syscall dispatch](../../ports/fuzix/upstream/Kernel/kdata.c) leaves
  syscall 72 as `_nosys` under that configuration.
- [Bundled select code](../../ports/fuzix/upstream/Kernel/select.c) is guarded
  by `CONFIG_LEVEL_2`, is absent from the active kernel object list, and limits
  `nfds` to 16 using single-word descriptor masks. It does not provide the
  current socket bridge's readiness integration.
- The RVM configuration permits 32 descriptors per process; the current native
  configuration permits 16. Supporting only the old mask would silently exclude
  valid RVM descriptors 16 through 31.
- The library has an internal `_select(int, uint16_t *)` declaration, but no
  public `sys/select.h` or working userspace `select()` wrapper in this port.
  `poll.h` defines constants and `struct pollfd`; that is not an implementation
  of `poll()`.
- [Socket I/O](../../ports/fuzix/ripple/sockets.c),
  [PTY I/O](../../ports/fuzix/ripple/pty.c) and
  [TTY I/O](../../ports/fuzix/upstream/Kernel/tty.c) already have blocking,
  queue and wakeup behavior to build on. Readiness waiters need to participate
  in those same state transitions.

Enabling all of `CONFIG_LEVEL_2` is not the implementation plan: that also
enables unrelated kernel facilities. Extract or gate the needed readiness
support explicitly and preserve existing job control.

## Interface and ABI decisions

1. Provide the conventional `select()` and `FD_ZERO`, `FD_SET`, `FD_CLR` and
   `FD_ISSET` API through `sys/select.h`. Reuse the existing `struct timeval`
   declaration in `sys/time.h`; do not introduce an incompatible duplicate.
2. Use a C16 `fd_set` with capacity 32 on both targets, represented by two
   16-bit mask words. Keep `FD_SETSIZE` distinct from the active process limit.
   The native kernel may reject `nfds > 16` until its descriptor limit grows;
   RVM must support its full 32. Do not shift a 16-bit `int` by 16 or more.
3. Keep the public interface independent of the internal syscall representation.
   Reuse syscall number 72 where practical, but replace the old 16-descriptor
   payload and its byte-offset assumptions with an explicit C16 layout. Record
   that layout in a shared header before implementation. No host `fd_set`,
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
Round positive sub-tick waits up to the supported timer resolution. Preserve
the distinction between a zero timeout and an indefinite wait. Convert large
timeouts with wide arithmetic and bounded chunks where necessary; avoid
overflow, early expiry, and deadline extension after spurious wakeups.

## Implementation work map

| Layer | Required work |
| --- | --- |
| Public libc | C16 descriptor sets/macros, declaration, wrapper, timeout validation/conversion and return count |
| Syscall ABI | Shared payload layout, syscall 72 wrapper/dispatch, bank-aware copy-in/out and pointer validation |
| Kernel wait core | Full descriptor range, registrations, timers, signals and balanced cleanup |
| Files/pipes | Immediate file results, pipe queue/EOF/broken-pipe transitions |
| TTY/PTY | Queue readiness, terminal mode semantics, hangup and wakeup integration |
| Sockets | Readiness queries and wake notifications from guest network state changes |
| Builds | Required sources/feature guards in RVM and native; no accidental dependency on all level-2 services |

The bundled implementation is a useful starting point, especially for inode,
pipe and TTY handling. Review its timeout units, mask width, user-copy offsets,
registration lifecycle and supported inode types before reusing it.

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
