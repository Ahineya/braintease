# Fuse-X networking and headless access

Networking and graphics can progress independently. If networking is ready
first, a headless Fuse-X kernel should be usable through a network terminal,
including a telnet-style development console.

## Existing foundation

The native runtime already participates in the shared Ethernet device protocol.
Guest protocol libraries provide networking outside the hosted Riplix integration.
Use the existing [networking guide](../networking/README.md) and
[host attachment instructions](../networking/running.md) for current capabilities
and commands.

The hosted kernel now enables the existing Riplix socket/inode bridge and
guest ARP/IPv4/ICMP/UDP/TCP stack. A generated build adaptation replaces the
bridge's reserved physical banks with shared hosted kernel allocation. Kernel
polling runs during idle and syscall boundaries; Riplix still owns descriptors,
blocking waits, terminal endpoints and process lifetime. The same transport sources serve both platforms. The existing socket and resolver libc supports the ten network tools.

## Headless terminal milestone

Provide a terminal endpoint independent of a graphical window. A network
connection can attach to an endpoint and start a guest shell, while a native
terminal window can use another endpoint through the same guest-side console
and terminal machinery.

Input waits must suspend their own process. Closing a connection must release
its endpoint and apply the chosen terminal hangup policy without ending the
kernel or unrelated sessions. Several terminal sessions should have independent
input and output streams.

`telnetd &` explicitly starts the development service. Connections get separate
Bourne shells on the sixteen guest PTYs, with job control and full-screen editors.
The init supervisor keeps the kernel alive when shells exit. The launcher also
supports `--headless --telnet` for a remote-only session, and `--detach`, `--attach`
and `--stop` for its persistent local console. Telnet provides no
authentication or encryption and belongs on a trusted development network.

The native integration test uses an independent raw-Ethernet peer for socket
ABI checks, UDP/TCP binary transfers, fork/dup descriptor sharing, ICMP in both
directions, DNS, HTTP, NTP, concurrent Telnet shells, remote jobs and Levee,
the guest Telnet client, repeated disconnect/reconnect, and offline persistence:

```sh
python3 fuse-x/test_network.py --executable fuse-x/build/fuse-x --compiler target/release/rcc
```

## Native controls

The proposed Network menu exposes disconnected, host-only, and shared host
attachment modes, with connection information. Reuse the existing host backend
and its authorization behavior. Report the guest interface and configuration
state accurately; host attachment status and a usable guest IP configuration
are distinct.

Treat a change in attachment as a link change. The guest side must respond to
it through its networking lifecycle, including address configuration and live
connections. Define whether changes can be applied while the session runs or
require a restart before exposing the control as live functionality.

The launcher exposes `--net auto|host|shared|off|/path/to/socket` and defaults to
`shared` with NAT internet access on macOS, or `off` on other platforms.
`auto` means host-only Ethernet on macOS and off elsewhere. `host` connects
the host and guest; `shared` adds NAT. The existing embedded vmnet helper requests
sudo authorization before interactive guest input handling begins. Noninteractive
launches attach lazily on first socket use. `RCC_NATIVE_NET_EAGER=1` requests
early attachment directly from the native runtime. The native process itself stays
unprivileged. `RCC_NATIVE_NET`, then `RIPPLE_NET`, supplies the launcher's default.
Guest configuration is `192.168.64.250/24`, gateway/resolver `192.168.64.1`.
Network menu controls, configuration persistence and live attachment changes
remain open. The local Ethernet fixture test does not require vmnet privileges
and does not establish that an arbitrary host's vmnet setup works.

After a working Ethernet helper fails or misses its five-second heartbeat,
the runtime reports link-down and reconnects in the background. Failed retries
back off to at most one attempt per thirty seconds. Old packet queues are
discarded; existing TCP connections fail and applications must reconnect.
Recovery never asks for a password inside the guest console. On macOS it can
restart the helper only while noninteractive `sudo` authorization is available;
otherwise the host log reports the authorization failure and the link stays down
until a retry can succeed. Initial startup failures remain cached, avoiding
repeated authorization prompts. These rules also apply to RVM's shared backend.

Local socket congestion is handled separately from link failure. Temporary
`EAGAIN`, `ENOBUFS` and interrupted sends do not revoke the helper's client.
Hello and heartbeat replies retry before Ethernet forwarding. A bounded
64-frame/64-KiB transmit queue retains frames in order while vmnet reports
buffer exhaustion or a zero-packet write; heartbeats continue during the wait.
A vmnet write or read that stays buffer-blocked for five seconds reports a
failure so ordinary attachment recovery can take over. Queue overflow drops
new Ethernet frames rather than growing host memory without limit.

Actual inbound Ethernet traffic also proves helper liveness if heartbeat
replies are delayed. Silence from a remote website alone does not establish
that the local Ethernet attachment failed and does not restart the NIC.
Helper errors are forwarded immediately into the session log as
`network helper: ...`, including vmnet's numeric error status. Recovery reaps
the helper before removing a socket left by an abnormal exit. This preserves
the cause in the session log instead of deleting the only diagnostic with the
old helper directory. Already running kernels keep their embedded helper;
these changes require rebuilding and restarting the kernel.

The helper regression uses the real forwarding loop with a fake vmnet API and
injected local socket faults. It requires macOS headers and local socket access,
but creates no virtual interface and needs no sudo:

```sh
python3 tools/network/test_vmnet.py
cargo test --offline --jobs 2 -p rvm --lib network::
```

The local console applies bounded backpressure when output outruns the attached
terminal. This can pause a noisy foreground program while preserving keyboard
input, including Ctrl-C, and the separate status/stop controls. Output bytes do
not trigger console detachment. Ctrl-] typed by the user still detaches; a
disconnected console continues logging with bounded rotation and replay.

The isolated broker flood regression needs no guest image:

```sh
python3 fuse-x/test_console_broker.py
```

Pass `--executable fuse-x/build/fuse-x` to additionally exercise a real guest's
`cat /dev/urandom`, Ctrl-C and clean shutdown using a temporary root image.

## Capacity and guest-local services

Both kernels now provide 64 internet endpoints, 64 active TCP
connections, 16 TCP listeners, 16 bound UDP ports and 128 compact TIME_WAIT
records. A separate pool of thirty-two Unix-domain
stream endpoints supports pathname services and `socketpair()` even offline.
These are guest endpoints governed by Riplix descriptors and filesystem
permissions; they do not bridge to Mac Unix sockets. They participate in the
same `select()` loop as PTYs and internet sockets. See [local IPC and
capacity](../networking/local-ipc.md) for buffer, backlog and lifetime rules.
