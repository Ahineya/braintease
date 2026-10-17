# Fuse-X networking and headless access

Networking and graphics can progress independently. If networking is ready
first, a headless Fuse-X kernel should be usable through a network terminal,
including a telnet-style development console.

## Existing foundation

The native runtime already participates in the shared Ethernet device protocol.
Guest protocol libraries provide networking outside the hosted FUZIX integration.
Use the existing [networking guide](../networking/README.md) and
[host attachment instructions](../networking/running.md) for current capabilities
and commands.

The hosted kernel now enables the existing FUZIX socket/inode bridge and
guest ARP/IPv4/ICMP/UDP/TCP stack. A generated build adaptation replaces the
bridge's reserved physical bank with shared hosted kernel allocation. Kernel
polling runs during idle and syscall boundaries; FUZIX still owns descriptors,
blocking waits, terminal endpoints and process lifetime. Port sources stay
unchanged. The existing socket and resolver libc supports the ten network tools.

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
Bourne shells on the four guest PTYs, with job control and full-screen editors.
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
