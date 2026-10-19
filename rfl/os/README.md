# RFL applications for Riplix and Fuse-X

RFL applications can be compiled on the host for both guest systems. They use
the guest libc, process table, filesystem and terminal. Neither compiler needs
to run in the guest. RFL Commander is the first modular terminal application
using this path.

## Standalone macOS platform

`rfl build SOURCE --os macos -o OUTPUT` builds a host terminal application.
Rift's convenience builder stages a relocatable distribution:

```sh
python3 rfl/apps/rift/build-macos.py
build/rift-macos/bin/rift /path/to/file.txt
```

This platform uses the native backend and the runtime's opt-in `host-tui`
feature. POSIX calls copy checked octet buffers to/from host descriptors;
directory/window/theme objects use opaque compatibility handles. The editor's
C engine keeps the compatibility ABI, while curses/themes compile with the
host compiler behind explicit adapters. Host pointers never enter RFL memory.
The builder selects a private runtime archive and adds the system termcap
library through `RCC_NATIVE_LIBS=ncurses` at link time.

In this mode `std/posix` accesses the host filesystem/environment and launches
host commands. Native directory capacity is 256 octets including NUL, while
guest builds keep 31. Host device/inode pairs map to stable process-local
32-bit identities without truncating host inode numbers. File sizes above the
32-bit metadata range return an overflow error. Process IDs are exposed as
`u32` on all platforms. The existing editor's file-size/tab limits are unchanged.
`RFL_DATA_DIR` defaults to `../share` relative to the executable's directory;
applications retain their own resource/configuration policy.

See [Rift's native guide](../apps/rift/README.md#standalone-macos) for controls,
resources and isolated host/terminal checks. This build path never opens a guest
disk, installs system files or depends on a guest kernel.

## Build and install

From the repository root, prepare the compilers and the selected OS using its
normal launcher build, then build an application:

```sh
cargo build --release -p rfl -p rcc-driver
target/release/rfl build rfl/examples/posix.rfl --os fuzix -o build/fuzix/rfl-demo.rxe
target/release/rfl build rfl/examples/posix.rfl --os fuse-x -o fuse-x/build/apps/rfl-demo.fxe
target/release/rfl build rfl/apps/commander/main.rfl --os fuzix -o build/fuzix/rfl-fm.rxe
target/release/rfl build rfl/apps/commander/main.rfl --os fuse-x -o fuse-x/build/apps/rfl-fm.fxe
```

`--build-dir PATH` selects an alternate OS SDK/build directory. Defaults are
`build/fuzix` and `fuse-x/build`; pass the actual directory when using the
Riplix Makefile's separate `ports/fuzix/build` default. `--compiler PATH` selects
RCC. Omitting `-o` places an `.rxe` or `.fxe` next to the input. Python 3 is
required, along with the existing OS toolchain (including `rasm`/`rlink` for
Riplix and a native linker for Fuse-X). Run from a checkout containing the SDK;
the compiler uses its build-time checkout path to locate the helper and libraries.

Both normal OS builds include `rfl-demo` and `rfl-fm`, with `mc` as a short
symbolic alias for Commander.
Their normal offline image install/upgrade step installs these under `/bin`,
preserving guest data.
Rebuild with `scripts/run-riplix.sh` or `scripts/run-fuse-x.sh` to make them
available in the corresponding system. A standalone `rfl build` only produces
the executable; it does not modify or restart an existing guest. Follow the OS
launcher's usual rules for stopping a running session before upgrading its disk.

Inside either guest:

```text
rfl-demo hello
mc / /tmp
```

The demo prints its arguments and PID, then writes and reads `/tmp/rfl-demo`.
Rift (`rift [FILES...]`) adds a programmer editor with tabs, syntax highlighting,
vi text objects, themes and guest program runners. See the [Rift guide](../apps/rift/README.md)
for controls, configuration and the plugin roadmap. Both normal OS builds include it.

Commander accepts optional left and right directories, defaulting to the
guest's current directory.

## Guest ABI and libraries

The build emits RCC IR using the Ripple backend for Riplix or the native backend
for Fuse-X, then links the matching guest entry point and libc. Ordinary RFL
values, collections, strings, closures, modules and owned resource cleanup use
the existing compiler. `println` and input use guest descriptors, so shell
redirection and pipes work. Returning from `main` sets the guest exit status;
an RFL runtime failure exits that process with status 1. It does not halt the
kernel. Builds stage output privately and replace the requested executable only
after a successful link.

`std/posix` exposes:

- `File`, with automatic close on drop, explicit fallible `close`, `try_clone`,
  binary `read`/`write_all`, text writes and 32-bit seek. Clones share file position.
- `open_read`, `create`, exclusive `create_new`, and `open_append`.
- Descriptor-based `read_bytes`, `write_all`, `write_text` and `STDIN`/`STDOUT`/`STDERR`.
- `metadata` using `lstat` (including permission bits), directory listing,
  mkdir/rmdir, unlink, rename, `set_permissions` and an `executable` check that
  follows symlinks and checks regular-file execute permissions.
- Guest arguments, environment, PID, current/change directory, command execution
  and exit. `run_command` invokes the **guest** shell and returns its encoded
  wait status, rather than a decoded exit code. `run_program(path, arguments)`
  uses literal arguments and guest fork/exec (with a guest-shell fallback for
  executable text), returns the same status format, and preserves Commander
  while foreground children receive terminal interrupts.

Operations return `Result<_, IoError>`; libc errors retain the guest errno.
Reads and writes retry `EINTR`, and writes handle partial progress. Paths are
octet strings without embedded NULs. A read is bounded to 65,536 octets;
directory snapshots are bounded to 4,096 entries, with the guest filesystem's
30-character names. Larger files can be streamed in chunks. File size and
offset limits remain those of the guest's 32-bit filesystem ABI.

`std/curses` exposes an owned `Screen` and independent owned `Window`s,
clipped text/fill operations, frames and line glyphs, dimensions, colors,
attributes, refresh, timeout, cursor, decoded keys, and suspend/resume. Dropping
the last screen/window owner restores the previous terminal screen and modes; libc's exit hook also restores it after an
RFL failure. `start()` uses raw input with a 100 ms key timeout. `NO_KEY` means
timeout and `CLOSED` means disconnected input. Function keys, navigation and
resize use the shared curses key codes. Text supports printable ASCII; other
characters display as `?`. Borders use DEC Special Graphics line glyphs, with
an ASCII fallback. The graphics terminal maps those glyphs onto its ROM font.

Styles are `u32`, combining `color_pair(n)` with `BOLD`, `REVERSE`, `UNDERLINE`
or `STANDOUT`. Call `screen.colors()`, then `screen.define_pair(n, fg, bg)`
for pairs 1–63. Colors 0–15 are ANSI ordinary/bright colors, 16–255 require a
256-color terminal, and -1 selects terminal defaults. `NO_COLOR` or `TERM=dumb`
disables color initialization; Commander retains reverse-video selection.
The graphics terminal currently displays the ordinary/bright 16-color palette.

`screen.window(rows, columns, row, column)` creates a floating layer. Stage the
background with `screen.stage()`, stage windows from back to front with
`window.stage()`, then call `screen.present()`. Read input using the top window's
`key()` so refreshing it preserves the other layers. Windows support local
drawing, borders, cursor placement, `move_to`, `scroll_region` and signed `scroll`
distances. Drop releases window memory; redraw lower layers after moving or
closing one. Independent windows are clipped safely after screen resize; the
application recreates/repositions dialogs when it receives `RESIZE`. Physical
consoles follow the host terminal's dimensions, including Fuse-X's persistent
console connection. Resizing an open dialog below its minimum closes it safely;
the main view shows a size notice until the terminal is enlarged.

`lib/ini` is a reusable, filesystem-independent INI parser shared with C.
`lib/tui_theme` provides generic theme validation, inheritance and configuration
layers; `lib/tui_curses` installs roles with application-defined monochrome
fallbacks. These optional libraries live in [`../lib`](../lib/), above the
standard runtime/service bindings. Their C APIs live in
[`../../libraries`](../../libraries/). `std/ini` remains a compatibility adapter.
INI also works in standalone RFL programs; the theme file loader uses guest libc.

The C files here are ABI adapters for opaque `DIR`/`stat`, libc macros and
curses' `stdscr`. Ownership, buffers, error results, widgets and application
behavior are implemented in RFL. There is no need to duplicate libc wholesale.

Use these libraries with `rfl build --os …`, rather than standalone `rfl run`.
The existing `std/mmio`, `std/host`, `std/net`, HTTP and hardware reactor path
targets standalone programs that own the devices. They cannot be used as guest
process services; the build rejects their MMIO/host dependencies. Guest socket,
select, monotonic timer/reactor and crypto wrappers are provided separately
below. Pure computational libraries and the existing language features remain
available.

## Commander controls and behavior

| Key | Action |
| --- | --- |
| Up/Down or k/j | Select an entry |
| Page Up/Page Down, Home/End | Page or jump through entries |
| Enter/Right/l | Enter a directory, run an executable, or view a non-executable file |
| F1 or Ctrl-A | About modal and shortcut reference |
| Left/Backspace/h | Parent directory |
| Tab/Shift-Tab | Switch active panel |
| F2 or Ctrl-R | Reload both panels |
| F3 or v | Preview the first 16 KiB of a text file, including executable scripts |
| F4 or e | Edit with VISUAL, EDITOR, or vi |
| r | Rename within the active directory; selection follows the new name |
| F5 or c | Copy to the other panel; destination is editable |
| F6 or m | Move; destination path is editable |
| F7 or n | Create a directory in the active panel |
| F8 or d | Delete a file, symlink or empty directory, after confirmation |
| F9 | Choose a session theme, or reload the configured theme |
| Ctrl-P | Toggle two panels / one active panel |
| Ctrl-O | Hide both panels and open the guest command prompt |
| F10, q or Ctrl-C | Quit and restore the terminal |

Prompts accept printable ASCII, Backspace, Ctrl-U to clear, Enter to submit,
and Esc/Ctrl-C to cancel. Overwrites and deletions require confirmation.
The floating text viewer wraps long lines at word boundaries by default without
changing the file. Up/Down and Page Up/Page Down scroll displayed rows; Home/End
jump to the start/end of the preview. Press `w` to toggle wrapping; Left/Right
scroll horizontally when wrapping is off. UTF-8 graphemes, wide characters,
combining marks, CRLF and tabs use the same text engine as Rift. Resize reflows
the preview while retaining its position. The preview remains limited to the
first 16 KiB and rejects binary NUL data; q/Esc closes it. Prompts,
confirmations and theme selection also use centered floating windows with
shadows. The theme selector supports arrows, Enter, Esc and first-letter jumps.
Directory panels retain their own selection/scroll positions, show a scrollbar
for long listings and distinguish active and inactive selections.

`python3 rfl/tools/test_commander_view.py --os both` checks wrapping, Unicode,
scrolling, wrap toggling and resize on disposable images of both real kernels,
and verifies that viewing leaves the source file unchanged.

F4 uses the first nonempty `VISUAL`, `EDITOR`, then `vi`. Both systems initialize
`EDITOR=/bin/vi` (the Levee alias). Editor values may contain shell arguments;
the selected absolute path is always passed as one quoted argument. Set/export
these variables in the guest shell or its startup configuration. Curses restores
the normal screen and input mode for the editor, then reloads both panels on
return, including when the command fails.

Enter uses executable permissions rather than file extensions. It follows
symlinks for the check, launches the selected program
in the active panel's directory and leaves its output visible until Enter
returns to Commander. Launch uses guest fork/exec with literal arguments,
without shell parsing of the pathname. F3 always requests viewing. This also works for executable
shell scripts. Nonzero command results are shown as the guest's encoded wait
status. The shared curses library uses the terminal's alternate screen;
quitting restores the previous shell screen, attributes, cursor and input mode,
without leftover panels or placing the shell prompt inside the file list.

Ctrl-O starts an interactive guest `/bin/sh -i` in the active panel's directory.
It uses the normal shell's Tab completion, history, startup settings, pipelines
and job control; `cd` and variable changes persist throughout that shell session.
Use `exit` or Ctrl-D on an empty line to return to the panels. An empty command
stays in the shell. Commander restores terminal ownership before resuming curses
and refreshes the panels; subshell directory/environment changes do not alter
the file manager's own state.

Copies stream regular files through an exclusively created sibling temporary.
The destination is replaced by rename only after copying and close succeed;
error paths remove the temporary, and same-file copies are rejected. Copies
preserve ordinary permission bits, including executability, and replace destination
symlinks rather than following them. Ownership, special mode bits and timestamps
are not preserved. Moves first try guest rename. Directory trees can be moved
with F6 or renamed with `r` within the same filesystem on both Riplix and Fuse-X.
These operations preserve the directory and file inodes, hard links, symlinks
and open files; moving between parents also updates `..` and parent link counts.
Moves into the directory's own descendants and replacement of nonempty
directories are rejected. On EXDEV, regular files use the
same complete-copy path and remove the source only after committing the copy.
If removing the source fails, Commander reports "Copied; source kept" with the
error, and both files remain. Cross-filesystem directory/symlink moves,
recursive copy/delete, symlink creation and search are not implemented. The minimum panel
size is 40 columns by 10 rows; smaller terminals show a size notice.

## Themes and configuration

Classic Blue is the default: ordinary dark blue panels, a cyan selection bar,
muted frames and gold function-key labels. It avoids bright-blue backgrounds.
Graphite and Monochrome are also installed. Color names follow the terminal's
palette, so their exact RGB appearance depends on the host terminal settings.

| Guest path | Purpose |
| --- | --- |
| `/usr/share/mc/themes/*.ini` | Named themes: `classic`, `graphite`, `mono`, and additional custom names |
| `/usr/share/mc/config.example.ini` | Commented configuration example |
| `/etc/mc.ini` | System-wide defaults; existing content survives upgrades |
| `$HOME/.config/mc/config.ini` | Optional per-user overrides; root uses `/.config/mc/config.ini` |

For example, create the user directories and copy the example, then edit it:

```sh
mkdir -p "$HOME/.config/mc"
cp /usr/share/mc/config.example.ini "$HOME/.config/mc/config.ini"
vi "$HOME/.config/mc/config.ini"
```

```ini
[theme]
base = classic
name = My Blue
borders = line

[colors]
selected = black, white
frame = white, blue
```

A named theme can inherit another named theme through `base`; overrides apply
after inheritance. User configuration without `base` overrides the effective
system theme. With `base`, it starts from that named theme instead. A complete
file can provide every role without any base (see `mono.ini`). Later assignments
win. Choose `configured` in F9 to reload system/user configuration; other F9
choices affect only the current session and do not rewrite configuration files.

Color roles are `text`, `frame`, `title`, `selected`, `inactive`, `status`,
`error`, `directory`, `dialog`, `input`, `accent`, `muted`, and `shadow`.
Each value contains a foreground, a background, and optional `bold`, `reverse`
or `underline`, separated by commas or spaces. Names are `black`, `red`, `green`,
`yellow`, `blue`, `magenta`, `cyan`, `white`, their `bright-` forms, or `default`;
numeric palette indices 0–255 are accepted. Use `borders=ascii` for plain borders.
Comments occupy their own lines starting with `;` or `#`.

Custom named theme files should use a new name rather than edit the managed
examples, which are refreshed during upgrades. Missing files are optional for
system/user configuration; missing named bases, cycles, excessive inheritance,
unknown keys, invalid colors and malformed files produce a visible notice.
Startup failures fall back to built-in Classic Blue; failed F9 changes retain
the active theme. Theme files are bounded to 16 KiB,
inheritance to eight files, and names to 24 letters, digits, hyphens or underscores.

## Modules and extension points

All Commander application source is in [`../apps/commander`](../apps/commander/):

| Module | Responsibility |
| --- | --- |
| `main.rfl` | Controller, command dispatch and operation results |
| `model.rfl` | Directory snapshots, sorting, paths, selection, scrolling and layout state |
| `keys.rfl` | Key-to-action mapping independent of rendering |
| `ui.rfl` | Panel layout and drawing |
| `widgets.rfl` | Input prompts, confirmations, viewer and console |
| `actions.rfl` | File operations, independent of curses |
| `format.rfl` | Text fitting and numeric formatting |
| `theme.rfl` | Semantic color/attribute styles and monochrome fallback |
| `theme_config.rfl` | Commander role defaults, paths and system/user precedence using companion libraries |
| `themes/` | Installed example themes and default system configuration |

INI parsing, color validation, configuration layers and base resolution are shared
with other C/RFL applications; see the companion library documentation above.
Theme roles map to shared curses color pairs independently of the directory
model. Layout state supports two, one or zero visible directory panels through
the single-panel and console modes; dialogs use independently owned windows.
More widgets can be extracted into a reusable RFL UI module as their contracts stabilize.
The application deliberately keeps filesystem actions independent of the screen.

## Validation

With both OS kernels and their normal tools built:

```sh
python3 rfl/tools/test_os.py --os both
make -C fuse-x test-rfl
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-rfl
python3 ports/fuzix/tools/test_directory_moves.py --os both
```

The integration runner builds its own RFL programs and uses disposable private
disks. It checks guest exit status and failure isolation, stdio/pipes,
arguments/environment, automatic descriptor cleanup, binary 70 KiB I/O across
a bank boundary, seek/dup, directory sorting and copy safeguards. It then drives
Commander through actual terminal input: About, navigation, viewer, the real
system editor, quoted filenames, rename, launching an executable, copy/move,
mkdir, delete/cancel, single-panel layout, guest commands and alternate-screen
restoration. Native tests move 70 KiB files across a private host mount in both
directions, preserve executable permissions, and check failed-copy/source-removal
safety.
A separate failure case checks curses restoration after an RFL panic. Theme
checks cover INI syntax, inheritance, cycles, complete themes, overrides, invalid
colors and user configuration. Live checks assert the rendered selection colors,
line borders, F9 theme changes, floating viewer dismissal, paged directory
scrolling, monochrome selection and shrink/recovery while a dialog is open.
Logs are saved as `/tmp/rfl-os-fuzix.log` and `/tmp/rfl-os-fuse-x.log`.
The directory-move runner separately checks tree identity, parent/link-count
updates, collision and permission errors, Commander F6/rename/cancel, and
persistence plus filesystem consistency after reboot, using disposable disks.

## Guest networking, readiness and crypto

`std/socket`, `std/poll` and `std/crypto` are OS services for both `--os fuzix`
and `--os fuse-x`. The builder detects their externs and links the C adapters
automatically. The implementation uses the existing RFL `std/shared` API;
this work does not modify the RFL compiler or its FP implementation.

### Shared asynchronous I/O

`std/poll::poller()` creates a **copyable registration handle**. Its copies
share the same engine and language-side waker table. `io.reactor()` acquires
that engine's sole driver, implementing `std/executor::Reactor`; another live
driver returns `SocketOsError(EBUSY)`. Dropping the driver permits a replacement
to resume pending operations. Dropping the original registration handle does
not invalidate its copies or futures.

A running future may retain `io`, resolve a name, connect, and register the next
read/write while the executor owns the driver. Registration never borrows the
reactor. The old requirement to register waits between executor calls is removed.
`std/shared` protects managed waker storage with short synchronous borrows;
no borrow crosses an await, and no managed RFL layout is exposed to C.

The engine lives in `os/poll.c`, `os/dns.c` and `os/socket.c`. C owns operation
records, descriptor interests, retries, transfer offsets, buffers, clock
accounting and deadline scans. Each turn makes bounded nonblocking progress,
then the kernel's `select` waits on descriptors and deadlines together. A turn
idles at most 100 ms; `Some(0)` never sleeps, and service work reduces the
remaining idle duration. At most 32 completed slot indices cross the C/RFL
boundary per turn. RFL collects their wakers, releases the shared table borrow,
then invokes the wakes. There are no callbacks into RFL for individual bytes
or packets.

The registration handle offers:

- `resolve_async(name, timeout_ms) -> ResolveFuture`, returning an IPv4 `Address`.
- `connect(address, port, timeout_ms) -> ConnectFuture`, returning an owned `Socket`
  after checking connection errors and peer state.
- `read(&socket, limit, timeout_ms) -> ReadFuture`, returning one received chunk,
  at most 32767 octets. Empty success means EOF, or a zero read limit.
- `write_all(&socket, bytes, timeout_ms) -> WriteFuture`, returning the number of
  bytes queued after writing the entire supplied buffer, at most 65536 octets.
  The buffer is copied once into C-owned storage; partial writes stay in C.
- `socket(&socket, READABLE|WRITABLE)` and `descriptor(fd, events)`, returning
  one-shot owned `Readiness` futures for lower-level event loops.

One asynchronous read and one asynchronous write may own an endpoint at once,
including across registry/socket clones. A competing same-direction transfer
or raw `try_read`/`try_write` returns `EBUSY`; independent read and write may
progress concurrently. Serialize application messages before sending them.
This prevents partial writes from silently interleaving separate messages.

Deadlines start on the operation's first poll and cover the entire operation,
including DNS retries, CNAME lookups and TCP fallback. Durations fit the
wrapping clock's half range (`0..2147483647` ms); zero expires immediately.
`move(wait).with_timeout(ms)` configures a raw readiness wait before polling.
Expiry returns `SocketTimedOut`. Drop/cancel synchronously unregisters pending
work, closes owned DNS/connect descriptors, and frees pending transfer buffers.
Cancellation does not undo bytes already sent. Completed receive data remains
owned until copied into the RFL result and the future is dropped.

Socket waits retain their endpoints. Descriptor waits own a duplicate, so
closing/reusing the caller's fd cannot redirect a pending wait. There are at
most 32 operations per registry, with actual network capacity also bounded by
guest socket/descriptor limits. Unpolled operations can be dropped safely;
DNS performs no network work before its first poll.

### DNS behavior

The C asynchronous resolver reuses guest libc's bounded DNS name/RR parsers.
It copies its own state instead of sharing libc's `hostent` response storage.
IPv4 literals and bounded `/etc/hosts` scans need no DNS query. It snapshots up
to three `nameserver` entries in `/etc/resolv.conf` (default `1.1.1.1`), retries
UDP twice per server with 2-second attempt intervals, and tries subsequent
servers on transient failure. Query IDs use the guest entropy device. Connected
UDP endpoints, transaction IDs and exact questions are checked before accepting
replies. Malformed/unrelated packets do not extend deadlines.

The resolver follows up to eight CNAME links, handles arbitrary answer order,
checks record ownership, rejects loops and unrelated address records, and
supports nonblocking TCP fallback for a matching truncated UDP response. TCP
length/payload transfers handle split reads and writes. Responses are capped
at 4096 octets and 128 records; invalid lengths fail without growing a buffer.
A turn consumes at most four UDP packets per lookup. Local configuration scans
are bounded to 64 lines for resolv.conf and 256 lines for hosts; these ordinary
filesystem reads remain synchronous. Network DNS waits are asynchronous and
cancellable. This first implementation supports IPv4 A lookups, with no IDNA,
AAAA, search suffixes or DNS cache.

`std/socket::resolve` retains its synchronous guest-libc behavior for existing
callers. New asynchronous code should use `io.resolve_async`.

### Low-level sockets and terminal input

`std/socket` also provides owned nonblocking IPv4 TCP endpoints, connected local
`socket_pair()` streams, shutdown and partial binary I/O. `try_clone` retains an
endpoint without consuming another descriptor. `try_read` returns `None` for
would-block/interruption and `Some([])` for EOF; `try_write` takes an offset and
reports partial progress. Raw operations transfer at most 32767 octets per call.
Raw connecting is `start_connect`, write readiness when pending, then
`finish_connect` to inspect `SO_ERROR` and peer state.

When combining curses with readiness, use nonblocking key reads and drain
buffered/decoded keys before waiting: descriptor readiness cannot see keys
already buffered inside curses. EOF is readable and must be handled explicitly.
The same readiness interface works for terminal, file and pipe descriptors;
descriptor watches consume a guest `dup` slot until released.

### Example and verification

The same registration handle is available both inside the async function and
to the independently driven reactor:

```rfl
import { Poller, poller } from "std/poll";
import { SocketError } from "std/socket";
import { block_on } from "std/executor";

async fn greeting(io: Poller) -> Result<unit, SocketError> {
    let ip = io.resolve_async("example.com", 5000)?.await?;
    let socket = io.connect(ip, 80, 5000)?.await?;
    io.write_all(&socket, [71, 69, 84, 32, 47, 13, 10], 5000)?.await?;
    let reply = io.read(&socket, 4096, 5000)?.await?;
    Ok({})
}

let io = poller()?;
let mut reactor = io.reactor()?;
let mut task = greeting(io);
let result = block_on(&mut task, &mut reactor);
```

`std/crypto` exposes the stateless crypto mailbox through the checked guest
`/dev/crypto` driver, plus `/dev/urandom`. It includes discovery, secret-exponent
2048-bit modular arithmetic, PBKDF2-HMAC-SHA512, AES-256-IGE and bounded SHA-256/512.
See [the device contract](../../docs/arch/crypto.md) for encodings and limits.
Telegram MTProto, authentication, session persistence and TUI are application work.

Run the disposable-kernel acceptance suite:

```sh
python3 rfl/tools/test_guest_services.py --target both
```

`tests/fixtures/guest_services.rfl` covers the original socket/readiness/crypto
API; `tests/fixtures/guest_async.rfl` covers the C asynchronous engine and dynamic
registration from running futures. Tests use private disks and an independent
Ethernet peer. Coverage includes concurrent binary transfers and DNS, timer
progress while DNS is pending, hosts/literals, NXDOMAIN/no-data, malformed and
unrelated replies, CNAME chains/loops, split TCP DNS and invalid lengths, UDP
retry/server failover, deadline expiry, exclusive transfers, driver ownership,
EOF, cancellation and descriptor reuse. No public Internet or Telegram account
is used, and the user's normal guest disk is never attached.
