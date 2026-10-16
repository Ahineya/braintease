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

Fuse-X still needs to connect its kernel, process scheduling, terminal
endpoints, and network services. A successful standalone TCP example does not
by itself validate FUZIX socket or terminal integration.

## Headless terminal milestone

Provide a terminal endpoint independent of a graphical window. A network
connection can attach to an endpoint and start a guest shell, while a native
terminal window can use another endpoint through the same guest-side console
and terminal machinery.

Input waits must suspend their own process. Closing a connection must release
its endpoint and apply the chosen terminal hangup policy without ending the
kernel or unrelated sessions. Several terminal sessions should have independent
input and output streams.

Acceptance requires an interactive shell, working input/output, orderly
disconnect, and a later connection that starts a usable terminal again. A
network console must remain usable without creating a graphics window.

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

The same settings must remain available to headless launches, without depending
on the menu bar. Final Fuse-X flags, configuration persistence, and the remote
terminal service protocol remain open.
