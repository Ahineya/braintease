# Boot services and console sessions

Fuse-X now uses its existing PID 1 to launch two independent userland paths:

```
/init (PID 1; reaps orphaned children)
  ├─ svcd --boot                  background service supervisor
  │    └─ explicitly enabled services and their required dependencies
  └─ console-session             first primary console only
       ├─ configured program     /bin/mux in the Fuse-X profile
       └─ exec /bin/sh            after that program exits or detaches
```

Graphical terminals and remote shells keep their existing startup paths. They
never consult the primary console policy. No shell startup file launches mux.
The host console broker is separate again: detach/reattach connects to the same
running guest terminal and does not create a new boot session.

## Activation and configuration

The normal `scripts/run-fuse-x.sh` build/install selects the `fuse-x` boot
profile. The next ordinary launch installs the mechanism into the default
Fuse-X image, preserving existing files and backing up changes as usual.
`--run-only` performs no installation; run once without it after upgrading.
Explicit `--disk` images change only when `--install-apps` is also selected.

Direct image construction and low-level test fixtures remain opt-in:

```
node fuse-x/image.mjs create ROOTFS.IMG --apps fuse-x/build/apps --boot-profile fuse-x
node fuse-x/image.mjs install ROOTFS.IMG --apps fuse-x/build/apps --boot-profile fuse-x
```

Without `--boot-profile`, the installer does not add activation files. RVM and
x64 installers do not import the Fuse-X profile. Existing x64 hardware/test
images therefore acquire no services or mux startup from this work.

The profile creates these editable, root-owned files, preserved on upgrades:

`/etc/boot/console.ini`:

```ini
[console]
program=/bin/mux
```

Set `program=` to start a plain shell instead. Optional `[arguments]` entries
`1` through `8` are literal arguments, with no shell expansion. Paths must be
absolute. The same ownership, regular-file, size and write-permission checks as
service definitions apply. Validate changes without launching anything:

```
console-session --config /etc/boot/console.ini --check
```

PID 1 consumes the policy only for the first successfully forked primary
console of that boot. Exiting/detaching mux falls through to a normal shell;
exiting that shell makes PID 1 start another plain shell. A new kernel boot
runs the policy again. Plain `mux` retains its existing live-session attachment
and saved-default-workspace behavior.

`/etc/boot/services.ini`:

```ini
[services]
enabled=
```

This intentionally enables no background applications. Definitions belong in
`/etc/services/NAME.ini`; installation alone does not activate them. Add names
to `enabled` only after validating the complete configuration. See the
[service manager reference](../ports/services/README.md) for schema, readiness,
dependency and restart semantics.

```
svcd --check --config /etc/services --profile /etc/boot/services.ini
svc --runtime /var/run/svcd status
svc --runtime /var/run/svcd start NAME
svc --runtime /var/run/svcd stop NAME
```

`svcd --boot` is reserved for PID 1's initial launch. It supplies the paths
above, creates a separate session with no controlling console, and uses a
private mode-0700 `/var/run/svcd` directory. Init redirects stdin to `/dev/null`
and stdout/stderr to `/var/log/svcd.log`. Individual service logs live in
`/var/run/svcd/NAME.log`. Rotation remains manual.

Boot does not wait for enabled services to become ready before opening the
console. Services express dependencies on readiness; console access remains
available when networking or another optional service fails.

## Failure, rescue and shutdown

Use `scripts/run-fuse-x.sh --run-only --rescue` to bypass both boot profiles
and reach a plain console shell. This does not edit the configuration. The
kernel also accepts `--rescue` directly. Explicit legacy `--gui`/`--telnet`
requests retain their existing meaning.

A missing launcher, invalid console policy, missing startup executable or
failed interactive program falls back to the shell. The launcher restores
terminal settings, disables mouse reporting and exits the alternate screen
even when the startup TUI was killed before running its cleanup handlers.
If the shell itself is unavailable, PID 1's existing console respawn path
remains the final fallback.

A failed or crashed supervisor is reaped and reported on the console. PID 1
does not restart it automatically: its former children may still be running,
so an automatic restart could duplicate services. Console access continues;
use rescue/reboot after investigating the logs and existing processes. There
is no adoption/reconciliation protocol yet. PID 1 reaps orphaned children and
includes surviving processes in final shutdown cleanup.

Because `/var/run` currently lives on the persistent root filesystem, abrupt
supervisor death can leave a control socket behind. Only the initial
`svcd --boot` path removes a stale socket, after validating the private runtime
directory and socket type/ownership. Ordinary supervisor invocations continue
to refuse an existing socket. Init makes one boot attempt; it does not run a
supervisor restart loop.

On guest shutdown, host stop, console EOF or one-shot completion, PID 1 first
sends TERM/CONT to the tracked supervisor and keeps reaping children while the
supervisor stops services in reverse dependency order. It then performs the
existing remaining-process TERM/KILL cleanup and filesystem shutdown. An
overall 90-second supervisor watchdog bounds this phase; if it expires, init
reports the deadline and proceeds with forced cleanup. Service authors should
budget the combined shutdown chain accordingly.

## x64 handoff after networking

The supervisor and console launcher are portable C userland; both also build
for RVM. The init hook resides in shared Fuse-X init code, but activation still
requires explicit files and installed executables. This work does not edit
x64 drivers, bootloader, hardware-image generation or networking initialization.

When x64 is ready, its agent should:

1. Package `svcd`, `svc` and optionally `console-session` using the x64 userland
   toolchain. Confirm sessions, process groups, close-on-exec pipes, Unix
   sockets, signals and child reaping on the actual target.
2. Supply a separate hardware boot profile. Keep hardware diagnostics and
   driver test images without activation files. Initially use a plain shell
   policy; selecting mux is a separate product decision.
3. Translate driver/interface readiness into a real networking task. DHCP
   configuration can be a successful one-shot task or a persistent lease
   manager with `ready=notify`; dependent services must not start on failed
   configuration. Distinguish address, DNS and internet reachability.
4. Integrate graceful service shutdown before platform reset/power-off and
   test failed startup, supervisor death, orphan reaping and rescue boot on
   hardware. Supply a boot-time rescue selection independently of the hosted
   command-line parser.
5. Decide whether `/run` should become a volatile filesystem, and later add
   explicit supervisor adoption/recovery rather than guessing from PID files.

Legacy Fuse-X GUI/Telnet switches are not migrated into INI service definitions
here. Live reload, enable/disable editing commands, log rotation and supervised
recovery are separate next milestones.

## Validation

`python3 ports/services/tests/test_boot.py` uses production image installation
on a private disk and performs genuine Fuse-X kernel restarts. It covers mux
startup/detach, later shell respawn, preserved configuration, dependency-ordered
shutdown, supervisor SIGKILL and orphan cleanup, stale-socket recovery on a new
boot, rescue mode, a killed TUI leaving raw/alternate-screen state, and invalid
configuration fallback. No normal or hardware image is opened by this test.
