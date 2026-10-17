# FUZIX and Fuse-X local sockets

Both execution targets implement guest Unix-domain stream sockets independently
of Ethernet. This is the local transport for a separate graphics server and
other guest services. It uses FUZIX descriptors, permissions, scheduling and
`select()`; it does not expose macOS Unix sockets to guest programs.

```c
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/select.h>
#include <string.h>

struct sockaddr_un address = {0};
address.sun_family = AF_UNIX;
strcpy(address.sun_path, "/tmp/nanox.sock");
int server = socket(AF_UNIX, SOCK_STREAM, 0);
/* Remove a stale name only when this service owns it. */
bind(server, (struct sockaddr *)&address, sizeof(address));
listen(server, 8);
/* select() can wait on server together with graphics, PTYs and other sockets. */
int client = accept(server, NULL, NULL);
```

`AF_LOCAL` aliases `AF_UNIX`. `socketpair(AF_UNIX, SOCK_STREAM, 0, fds)` creates
an unnamed connected pair. `read`, `write`, `send`, `recv`, `shutdown`,
`getsockname`, `getpeername` and `getsockopt(SOL_SOCKET, SO_ERROR)` work on these
streams. `recvfrom` can return the peer's name; an explicit destination on a
stream send is rejected. Message flags, local datagrams, abstract names,
ancillary messages, descriptor passing and peer-credential queries are not
implemented. Protocol must be zero.

## Names and ownership

`sys/un.h` defines `sockaddr_un` with a family cell and 108 path cells. Lengths
are C16 characters, including the family; use `sizeof(address)` or the family
plus the terminated path. Paths can contain up to 107 characters, with the
existing filesystem's 30-character component limit. Empty/abstract names are
rejected. Returned unnamed addresses contain just the family. Address output
is truncated to the supplied capacity while returning the actual length.

Binding creates a filesystem socket node with mode `0777 & ~umask`. Parent
traversal/creation permissions apply; connecting requires write permission on
the socket node. A live binding retains an inode reference: relative paths,
`..`, hard links and renames identify the same service. Unlink removes the name
without breaking established connections; a new listener may bind that name
independently. Closing a listener does not unlink its name. A stale node refuses
connections until explicitly unlinked and rebound, including after reboot.
Guest socket nodes cannot be created in Fuse-X's Mac folder export.

Connections complete immediately when backlog space exists. A full backlog
makes a nonblocking connect return `EAGAIN`; a blocking connect sleeps until
accept frees space, the listener closes, or a signal interrupts it. Descriptor
allocation failure at accept retains the queued connection for a later retry.
Listener closure disposes of unaccepted endpoints and wakes their clients.
Endpoint generations prevent a blocked connect from following a reused slot.

## Streams and readiness

Streams carry full 16-bit C16 characters; they do not impose internet sockets'
one-octet-per-cell restriction. They preserve order and may return partial
reads/writes. Each endpoint has a 1,024-cell receive ring. Full rings block
writers or return `EAGAIN` in nonblocking mode. Reading frees space and wakes
writers. Use an explicit framing convention for messages; write boundaries
are not message boundaries.

Read readiness means queued data, EOF, read shutdown or a pending error.
Write readiness means buffer space or an error that can be reported immediately.
Listener read readiness means at least one pending accept. Readiness does not
consume data, clear errors or accept connections. Exception sets remain empty.
Use nonblocking descriptors and handle `EAGAIN` when several processes share a
stream. `shutdown(SHUT_WR)` lets the peer drain queued data before EOF;
`SHUT_RD` discards the local receive queue and makes peer writes fail with
`EPIPE`/`SIGPIPE`. Final close wakes the peer; dup/fork references keep the
endpoint alive until its last open-file reference closes. Signals interrupt
blocking operations with `EINTR`.

## Capacity and memory

| Resource | FUZIX/Ripple and Fuse-X |
|---|---:|
| Internet socket endpoints, all protocols | 16 (previously 8) |
| Active TCP connections across listeners and outbound connections | 16 (previously 4) |
| TCP listeners | 4 (previously 2) |
| Connection slots per TCP listener | 4 |
| Bound UDP ports | 4 (unchanged) |
| Local endpoints, including listeners and pending accepts | 32 |
| Pending connections per local listener | Up to 8 |
| Receive storage per local endpoint | 1,024 C16 cells |

An established local connection uses two endpoints. Internet and local pools
are independent but share the system inode/open-file tables. Descriptor limits
remain 32 per Ripple process and 16 per native process. Closing internet sockets
may retain a slot during protocol cleanup; local slots are released immediately.
The compact TCP TIME_WAIT table remains 32 entries.

Ripple reserves bank 5 for protocol state, bank 6 for internet endpoints and
bank 7 for local endpoints. Each table is checked at compile time to fit one
64,000-cell bank. These supervisor banks do not reduce the existing user page
pool or overlap graphics storage. Fuse-X allocates the same tables in shared
kernel memory. `RIPPLE_FUZIX_NETWORK` selects the larger TCP context consistently
for the bridge and every linked network implementation unit; standalone
runtime/RFL contexts retain their existing four-connection/two-listener defaults.

## Verification

After rebuilding the kernel and guest libc:

```sh
python3 ports/fuzix/tools/test_select.py --local
python3 ports/fuzix/tools/test_select.py --local --native
python3 ports/fuzix/tools/test_select.py
python3 ports/fuzix/tools/test_select.py --native
```

All tests use disposable images. Local tests run without a NIC and cover full
C16 data, circular-buffer backpressure, half-close, EOF, dup/fork, permissions,
name aliases, unlink/rebind, pending-client cleanup, interrupted select, backlog
waits, accept descriptor exhaustion, socketpair allocation rollback, bank-spanning
addresses/buffers and blocked-connect listener reuse. A real second boot checks
stale pathname refusal and unlink/rebind; native managed installation and the
offline checker preserve socket nodes without treating them as data files. The
network readiness suite also verifies four listeners and sixteen simultaneous
outbound connections distributed across two guest processes, exchanges data
on every connection and checks endpoint exhaustion. Internet fixtures use a
local Unix datagram socket and require no internet connection.
