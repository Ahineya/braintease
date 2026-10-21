# Standalone service management

`svcd` is a C service supervisor; `svc` is its local control client.
`console-session` applies an interactive startup policy and falls back to a shell.
They share the repository's INI reader and compile for the development host,
native Fuse-X, and Riplix/RVM. Standalone builds install or enable nothing.
The normal Fuse-X launcher now selects an explicit boot profile with an empty
service activation list and mux on the first primary console only. RVM/x64
installers are unchanged. See [boot integration and x64 handoff](../../docs/boot-services.md).

A future x64 network stack can expose an interface to a DHCP/static-address
configuration program. That program can complete a one-shot initialization task,
or send readiness from a persistent lease manager; dependent services can then
start. Drivers and the network stack still implement networking. An address,
working DNS and internet reachability are distinct conditions. Local boot and
console access should not require a successful external internet probe.

## Build and isolated acceptance tests

From the repository root:

```
python3 ports/services/build.py
python3 ports/services/tests/test_services.py
python3 ports/services/build.py --target native --helpers
python3 ports/services/tests/test_guest.py --target native
# Requires a current Fuse-X kernel, mux, shell and native boot tools:
python3 ports/services/tests/test_boot.py
python3 ports/services/build.py --target ripple --helpers
python3 ports/services/tests/test_guest.py --target ripple
```

Outputs go only into `build/services/{host,native,ripple}`. `--helpers` builds a
synthetic test program, never installed in a normal image. Guest tests construct
private disposable disks and manually run fake services there; they do not touch
hardware disks or run x86 builds. Cross-builds reuse the corresponding existing
userland SDK; kernel builds are not requested. The extra Ripple makefile declares
only explicit service-tool targets and is not included by normal builds.

The host suite uses temporary Unix sockets and child processes. It covers
configuration validation, dependency cycles, disabled defaults, readiness,
literal Unicode arguments, automatic retry bounds, dependency recovery,
ordered stop, TERM/KILL deadlines, slow clients, duplicate supervisors and
explicit profile activation, and 100-definition configurations with long lists
and responses exceeding the old fixed buffer. Allocation-failure checks verify
that partial configurations are cleaned up and report memory exhaustion. Guest
suites exercise 76 definitions through the actual Fuse-X and RVM process/socket/INI
paths and check their disposable filesystems after shutdown.

## Configuration and enablement

The manager has no implicit configuration location and **no enabled services
by default**. Definitions and activation profiles are separate:

```
svcd --check --config /path/to/definitions
svcd --config /path/to/definitions --runtime /path/to/private-runtime
svcd --config /path/to/definitions --runtime /path/to/private-runtime --profile /path/to/boot.ini
```

`--check` parses the complete configuration and prints a deterministic dependency
plan without creating a socket or starting programs. Without `--profile`, the
running manager starts nothing until explicitly requested through `svc`.
Profiles are read only at startup; live configuration reload is not implemented.

A definition is named `NAME.ini`, where NAME has 1–24 letters, digits, hyphens
or underscores. Definitions and dependency lists are allocated dynamically,
with no fixed service-count limit. Allocation failure is reported before any
service starts. Files must be regular, owned by the manager's user, at most
8192 input cells, and not group/world writable. Each INI line can use the full
file budget, including dependency and profile lists. The definitions directory
has the same ownership/write restrictions.
Symlink definitions are rejected. Duplicate/unknown settings, missing required
fields, argument gaps, unknown dependency names and cycles fail validation
before any service is started. Example files end in `.ini.example` so they are
not accidentally discovered as active definitions.

```ini
[service]
type=simple
program=/absolute/path/to/daemon
ready=notify
restart=on-failure
restart_limit=5
restart_delay_ms=1000
start_timeout_ms=10000
stop_timeout_ms=2000
directory=/
after=logger
requires=network-config

[arguments]
1=--foreground
2=an argument with spaces
```

Arguments are numbered 1–8 without gaps, up to 127 input cells each. They are
passed directly to `execve`; there is no shell expansion, interpolation or
implicit command evaluation. An explicit `/bin/sh` program is available for
scripts. INI matching outer quotes are stripped by the shared reader. Programs
and working directories must be absolute paths, up to 255 cells.

The example above describes a future daemon that implements readiness and
requires definitions named `logger` and `network-config`; these services are not
provided or enabled by this package.

A profile selects services explicitly:

```ini
[services]
enabled=network-config remote-shell
```

Required dependencies are included automatically. `after` alone never enables
another service. The shipped sample profile enables nothing.

## Service types, readiness and dependencies

- `type=oneshot`: a task becomes `done` only after successful exit. A failed
  configuration command cannot unblock its dependents. `ready=notify` and
  `restart=always` are invalid for this type.
- `type=simple` (default): a foreground process is supervised until it exits.
  It must not daemonize away from the manager's tracked process.
- `ready=exec` (default): the manager observes successful `execve` through a
  close-on-exec pipe. This means launched, **not application-ready**. Exec errors
  are recorded as failures, including for one-shot services.
- `ready=notify`: a simple service stays `starting` until it writes exactly one
  `R` octet to the descriptor named by `SVC_READY_FD`, then closes it. Missing,
  malformed or late readiness fails startup; no arbitrary sleep is used to
  assert readiness. Send the octet only after the application's actual setup
  has succeeded.
- `requires`: starts the dependency first and waits until it is running/ready
  or successfully completed. Failed dependencies block startup. Loss of a
  required running service stops its dependents; they can restart when it
  recovers. Explicitly stopping a provider also disables its required subtree.
- `after`: orders startup if both services are selected. It imposes no lifetime
  dependency; stopping an ordering-only predecessor leaves the other running.
  During a joint shutdown, ordering is reversed and predecessors wait for their
  stopping dependents to exit.

The manager has one event loop, with at most four concurrent startup attempts
to keep descriptors and memory bounded on the guest systems. It uses no
per-service supervisor processes. A successful start request is asynchronous;
query status to observe readiness or a failure.

## Lifecycle and control

```
svc --runtime /path/to/private-runtime status
svc --runtime /path/to/private-runtime status NAME
svc --runtime /path/to/private-runtime start NAME
svc --runtime /path/to/private-runtime stop NAME
svc --runtime /path/to/private-runtime restart NAME
svc --runtime /path/to/private-runtime shutdown
```

`shutdown` stops the managed services and exits this supervisor; it does not
shut down the OS. SIGINT/SIGTERM to the manager request the same orderly stop.
The control socket lives in a private mode-0700 directory owned by the manager's
user. A second supervisor cannot claim an existing socket. After an unclean
manager death, inspect/stop any old services before manually removing a stale
socket; the tools do not guess process ownership from stale PID files.

Status streams one row at a time, so reply storage does not grow with the number
of definitions. Status reports state, tracked PID, desired activation, retry
count and last exit code/signal. States are `down`, `waiting`, `starting`, `running`, `done`,
`stopping`, `backoff`, `failed` and `blocked`.

`restart` accepts `no` (default), `on-failure`, or `always`. The retry budget is
per activation, default five automatic retries, with a default 1000 ms delay.
Explicit restart resets the budget; starting a failed service retries its
required dependency closure as well. `restart_limit` permits 0–100. Startup,
stop and retry deadlines permit 100–60000 ms. Stopping sends TERM and CONT,
then KILL after the stop deadline. Dependency shutdown waits for dependent
processes to exit before stopping providers.

Service stdin is `/dev/null`; stdout/stderr append to `NAME.log` under the
private runtime directory. Logs currently require manual rotation. Services
inherit a small fixed PATH/HOME/SHELL/LANG environment and run as the manager's
user; privilege changes and custom environments are not implemented.
Process groups provide cleanup of descendants that remain in the service's
group. This is not containment of children that deliberately create new
sessions; such daemons need their own shutdown handling or future stronger
kernel process containment.

## Boot integration

The tools remain ordinary userland, not PID 1. Fuse-X init explicitly invokes
`svcd --boot` only when `/etc/boot/services.ini` exists, and keeps the primary
console policy separate. Normal standalone command-line behavior still starts
nothing without a profile or an explicit start request. The boot supervisor is
not automatically respawned after failure; runtime state is in memory and
there is no process-adoption protocol after SIGKILL.

See [boot services and console sessions](../../docs/boot-services.md) for
installation, rescue mode, persistent configuration, stale-socket recovery,
ordered shutdown and the x64 integration handoff. No network adapter, Telnet
service or future SSH definition is enabled by the shipped service profile.


### Optional recurring jobs

The Unix utility batch installs a disabled `/etc/services/cron.ini` definition.
`cron`/`crond` stays in the foreground, reads `/etc/crontab` each minute and
invokes `atrun` for `/var/spool/at`. Add `cron` to a boot profile's enabled list
explicitly, or start it with `svc --runtime /var/run/svcd start cron` on an
existing supervisor. Service output follows the normal `cron.log` path; at-job
output remains in `/var/spool/at/output`. Installing the definition never starts
it. See the [Unix userspace guide](../fuzix/README.md#tree-option-parsing-and-scheduled-jobs)
and `man cron` for schedule syntax, ownership rules and recovery behavior.
