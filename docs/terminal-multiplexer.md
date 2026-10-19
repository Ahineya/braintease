# mux: terminal workspaces

`mux` is a shared C application for FUZIX/Ripple and native Fuse-X. It runs
independent shells and full-screen applications in tabs and split panes inside
an existing terminal. It works through the physical console, graphical
terminals and Telnet; it has no graphics or VM-specific dependency.

## Getting started

Normal application builds install `/bin/mux` and `man mux` on both systems.
Run `mux` to create the default session `main`, or attach it if already running.
Use `mux new work` for another named session. An optional command replaces the
first pane's shell, for example `mux new browse lynx`.

Commands use a prefix: press **Ctrl-B**, release it, then press the command key.
Press Ctrl-B twice to send that control character to the application.

| After Ctrl-B | Action |
|---|---|
| `?` | Show keyboard help; any key closes it |
| `c` | New tab |
| `n`, `p`, `1`–`8` | Next, previous, numbered tab |
| `,` | Rename tab; Ctrl-U clears the name |
| `%` | Split left/right |
| `"` | Split top/bottom |
| Tab or `o` | Next pane |
| `h`, `j`, `k`, `l` or arrows | Focus left/down/up/right |
| `+`, `-` | Grow/shrink the pane at its nearest split |
| `z` | Zoom the pane or restore its layout |
| `[` | Enter scrollback and copy mode |
| `]` | Paste the internal clipboard |
| `x` | Close pane, with confirmation |
| `d` | Detach and leave applications running |
| `r` | Redraw |

Ordinary Tab, function keys, Ctrl-C and Ctrl-Z go to the focused pane. Each pane
has its own shell job control: `jobs`, `fg` and `bg` operate on that shell's jobs.
The header shows tabs and background activity; the footer identifies the
focused pane. Small terminals temporarily show only the focused pane when the
split tree cannot fit. Enlarging the terminal restores the layout. At 80 columns,
a left/right split is approximately 39/40 columns; use zoom or top/bottom splits
for applications that need a wider screen.

In scrollback, use arrows or `j`/`k`, PageUp/PageDown or `b`/`f`, and `g`/`G`.
Space marks the start of a line selection; move and press `y` to copy it.
Without a mark, `y` copies the current line. `/` searches backward through the
retained lines, wrapping; `n` repeats. Escape or `q` returns to the application.
Copying also leaves scrollback. This clipboard is private to the session.

## Sessions and configuration

`mux list` lists session names. `mux attach work` reconnects to a session;
`mux kill work` terminates it and its panes, including an attached session.
Only one client may attach at once. A second attachment is rejected without
disturbing the first. Closing a pane explicitly terminates the processes in its
session, including background and stopped jobs; unsaved work is discarded.

The server keeps PTY masters, terminal models and history independently of the
attaching terminal. A disconnected client does not terminate the server.
Sessions last while the guest operating system runs; this is not process
checkpointing across shutdown or reboot.

Read `$HOME/.muxrc` when starting a new session:

```
# Retained lines per pane, 0..1000.
history=200
# A lowercase letter selects its control character.
prefix=b
# An absolute executable path, without arguments.
shell=/bin/sh
```

Session sockets live in `/tmp/mux-UID/NAME`. The directory must be owned by the
calling user and have mode 0700; sockets use guest Unix-domain IPC. Names allow
1..24 letters, digits, hyphens and underscores. Creation recovers stale socket
nodes after a reboot but refuses to replace other filesystem entries.

## Architecture and performance

The same executable implements a session server and an attaching client. The
server creates a separate session and closes inherited outer terminal
references. Every pane creates another session and acquires its own controlling
PTY before executing its application. The client changes only its outer
terminal to raw mode and restores terminal settings and display modes on exit.

The local stream protocol carries length-framed keyboard and resize messages
(including the outer terminal erase character);
the server sends composed ANSI output. Reads/writes are nonblocking and partial
transfers retain their pending data. Queues are bounded. Read interest is
removed when downstream capacity is exhausted rather than dropping input or
spinning. The RVM console also retains host input when its guest queue is full,
so backpressure extends through the outer terminal during large pastes. PTY output is consumed for inactive tabs as well as visible ones.

A bounded round of PTY reads prevents a continuous writer from starving input
or other panes. `select()` sleeps for readiness, child termination or deadlines.
Physical-console size is periodically queried because the current ABI discovers
host geometry through `TIOCGWINSZ`; graphical/Telnet PTYs also deliver SIGWINCH.

Each pane uses the shared `ports/graphics/terminal.c` emulator, keeping clear,
scroll, cursor movement, replies and alternate screens within its own grid.
Visible dirty rows are composed into a retained outer grid; the renderer sends
only changed spans with batched styles and DEC line-drawing character sets.
History records lines scrolled out of the primary full-screen region, excluding
alternate screens and partial-region scrolling. Search is literal ASCII.

The shared terminal engine owns two dynamically sized screens. Resize allocates
replacement storage before publishing it, preserving both screens on failure,
then retains the top-left overlap and clamps saved cursors. It does not reflow
wrapped text. Maximum model geometry is 240×100; existing GX/Nano-X terminals
keep their separate 80×30 graphics limits. Underline/blink and cursor-shape
state are retained; extended colours are approximated on the existing 16-colour
palette. Full Unicode and complete xterm emulation remain outside this version.

## Resource preparation and limits

Both kernels provide **16 PTY pairs**, named `/dev/ptyp0` through `/dev/ptyp9`,
then `/dev/ptypa` through `/dev/ptypf`, with matching `ttyp` slaves. Existing
names/minors remain unchanged. Shared header definitions drive both image
builders, avoiding separate hard-coded installation counts. All existing PTY
consumers use the same hexadecimal naming convention.

Fuse-X uses existing shared kernel allocation for its PTY queues, preserving
512-character input/output buffers without overflowing its static global bank.
Both systems support **32 descriptors per process**, including high-descriptor
selection and close-on-exec flags. There are still 64 process slots, 192 system
open-file entries and 128 cached inodes. Every shell/application and other OS
service participates in those shared budgets.

A session supports eight tabs and twelve panes in total. Other GUI terminals,
Telnet sessions and other mux servers share the global PTY pool. An outer GUI
terminal running mux consumes a PTY of its own. Creation can fail before the
session limit when memory, processes or shared PTYs run out; existing panes
remain usable. The internal clipboard holds 8191 characters. History allocation
is bounded and grows as lines are retained, rather than allocating full history
screens for every new pane.

## Verification

```
make -C ports/fuzix BUILD="$PWD/build/fuzix" test-mux
make -C fuse-x test-mux
```

The Python integration driver uses real guest kernels, a real outer PTY and
private disposable disks. RVM runs in JIT mode by default. The capacity probe
covers every PTY name, allocation above descriptor 15, high-descriptor selection
and close-on-exec. Interactive checks cover tab isolation, split layouts,
resizing, job control, existing editors/file manager/browser, history copying,
6000-character input bursts, hidden-tab output, detach/reattach, hangup survival,
all twelve panes/eight tabs, custom configuration and teardown/reuse. Private
filesystems are checked after clean shutdown. The portable VT tests cover fragmented escape
sequences, both-screen resizing, large geometry and parser bounds.

### Results for this implementation

The mux acceptance suite passes on both kernels, including the sustained-input
and clean-filesystem checks. RVM graphics regressions pass with sixteen PTYs;
the portable terminal model passes undefined-behavior checks, and the disk
suite passes all 19 tests. Both graphical terminal applications rebuild for
both execution targets.

The broader native graphics suite was also attempted. It reaches the GUI child
survival section but times out waiting for `NXTERM_READY` in the redirected
`/tmp/gui-child.log`; that full suite is not counted as passing. This is separate
from the successful native mux/PTY acceptance run and needs follow-up.
