# mux: terminal workspaces

`mux` is a shared C application for Riplix and native Fuse-X. It runs
independent shells and full-screen applications in tiled and floating desktop tabs inside
an existing terminal. It works through the physical console, graphical
terminals and Telnet; it has no graphics or VM-specific dependency.

## Getting started

Normal application builds install `/bin/mux` and `man mux` on both systems.
Run `mux` to attach the live session `main`. If it is not running, mux restores
the last saved default workspace, or starts a fresh shell if none exists.
Use `mux new main` to start fresh explicitly, or `mux new work` for another named session. An optional command replaces the
first pane's shell, for example `mux new browse lynx`.

Commands use a prefix: press **Ctrl-B**, release it, then press the command key.
Press Ctrl-B twice to send that control character to the application.

| After Ctrl-B | Action |
|---|---|
| `?` | Show keyboard help; any key closes it |
| `c` | New tiled tab |
| `f` | New floating desktop tab |
| `w` | New shell window in the desktop tab |
| `e` | Run dialog: open a program in a desktop window, or a new tiled tab |
| `M` | Minimize the focused desktop window |
| `m`, `v` | Move / resize the focused window; arrows or `hjkl`, Enter/Esc finish |
| `t` | Rename the focused desktop window |
| `n`, `p`, `1`–`8` | Next, previous, numbered tab |
| `,` | Rename tab; Ctrl-U clears the name |
| `%` | Split left/right |
| `"` | Split top/bottom |
| Tab or `o` | Next pane |
| `h`, `j`, `k`, `l` or arrows | Focus left/down/up/right |
| `+`, `-` | Grow/shrink the split, or the desktop window |
| `z` | Zoom the pane or restore its layout |
| `[` | Enter scrollback and copy mode |
| `]` | Paste the internal clipboard |
| `x` | Close pane, with confirmation |
| `d` | Detach and leave applications running |
| `r` | Redraw |
| `S` | Save the named session and update the default workspace |
| `C`, `D` | Override the focused pane’s restart command / directory |
| `A` | Clear those overrides and use automatic foreground capture again |

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

## Floating desktop tabs

Press **Ctrl-B f** to create a desktop tab, marked `[D]` in the tab strip.
It starts with one shell; **Ctrl-B w** opens another overlapping window.
Tiled tabs remain available with **Ctrl-B c**. The two kinds of tab can coexist
in one session, sharing the same twelve-pane/eight-tab limits and PTY pool.
`%` and `"` also create a window when used in a desktop tab.

- Drag a title bar to move its window. Drag a side, bottom edge or corner to
  resize it. The title buttons minimize (`[_]`), maximize/restore (`[+]`/`[-]`), and close
  immediately (`[x]`). Ctrl-B x still asks for confirmation. Side and bottom borders
  use the normal terminal background. Mux’s top/bottom bars are dark gray;
  window title strips are lighter gray, with a blue text accent on the active window.
- Click a window to focus and raise it. The task bar above the normal status
  line lists every window in stable creation/slot order; clicking a title raises
  that window even when it is completely covered or minimized. Cycling with Ctrl-B Tab/o
  raises the next window too. Ctrl-B `hjkl`/arrows choose a window in that
  direction by its center, including overlapping windows.
- Ctrl-B **m** enters Move mode; Ctrl-B **v** enters Resize mode. Then use arrows
  or `hjkl` in one-cell steps, finishing with Enter, Escape or Ctrl-C. These modes
  consume their controls rather than sending them into the application.
  Ctrl-B **+/-** grows/shrinks both dimensions. Ctrl-B **z** maximizes/restores.
- Ctrl-B **t** sets a manual window title. By default, titles follow the program
  launched by the system shell (for example `rift` or `lynx`), returning to
  `shell` at its next idle prompt. A manual title takes priority; clear it with
  Ctrl-U and Enter in the title prompt to return to automatic titles.
  Ctrl-B **,** still renames the whole tab. Ctrl-B **!** moves the running pane
  into its own tiled tab, without restarting its process.

Mouse reports inside a window's content are translated into that child's local
coordinates and respect its requested reporting modes. A child drag retains its
owner even when it crosses another window. Window-manager drags are captured
until release; resize, detach, tab changes and pane replacement cancel capture.
**Shift-drag remains terminal text selection**, including over a desktop tab.

Preferred window rectangles survive a temporary smaller outer terminal. Windows
are clamped to the available desktop without overwriting their saved geometry;
terminals too small for a frame temporarily show only the focused terminal.
**Ctrl-B M** minimizes a window without stopping its program. Minimized windows
are not composed or hit-tested, but their PTYs continue to run and collect output.
Their taskbar text is dimmer, with the same label and slot width. Click a taskbar
button to restore/raise it; Ctrl-B Tab/o also cycles through hidden windows and
restores the selected one. Minimizing the last visible window leaves an empty
desktop; ordinary keys/pastes are not sent into hidden applications. The Run
dialog, taskbar and prefix controls remain available. Minimized state and manual
title overrides are included in saved/default workspaces. Images remain planned
separately.

### Run a program

**Ctrl-B e** opens a centered Run dialog. Enter a program name or command with
arguments, such as `rift '/mnt/host/notes.js'`. Enter or the Run button launches
it; Escape or Cancel dismisses the dialog. Backspace removes a Unicode grapheme,
Ctrl-U clears input, and bracketed paste inserts text without executing a pasted
newline. Small terminals use the footer as the input field.

In a desktop tab, Run creates a new floating window. In a tiled tab, it creates
a new mux tab named after the program. The launch inherits the selected pane's
reported directory, or its configured directory when overridden/unavailable.
The command runs through the configured shell, returning to an interactive shell
when it finishes; launch errors remain visible there. Run commands are retained
as restart recipes. Automatic window titles use the existing shell launch reports,
so there is no periodic process polling or extra rendering cost per character.

This takes inspiration from [Twin](https://github.com/cosmos72/twin), whose
terminal window manager supports overlapping windows. It extends mux's existing
PTY/session model and does not require a new application protocol or a Twin port.
Existing TUI applications work in a desktop window without changes.

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

## Save once, return after reboot

**Ctrl-B S** saves both the current named session and `$HOME/.mux/default`.
`mux save NAME` does the same from inside a pane, another terminal or a script,
whether the session is attached or detached. The default is an independent,
atomic copy of the most recently saved workspace. Named saves remain available
with `mux saved` and `mux restore NAME`.

A plain **`mux`** attaches to live `main` first. Otherwise it restores the default
workspace as `main`, including tiled/desktop tabs, rectangles, titles, stacking,
focus, zoom, shell configuration and per-pane restart recipes. With no saved
default it starts a fresh shell. `mux new NAME [COMMAND ...]` and an explicit
`mux COMMAND ...` bypass automatic restore. An invalid default produces an error;
`mux new main` lets you start fresh without overwriting it.

The interactive system shell reports foreground external launches and idle
prompts through a private, bounded terminal OSC. At save time mux checks that
the reported process group still owns the pane's foreground terminal. It saves
shell-quoted arguments and the command's launch directory; an idle shell saves
its current directory. Thus `rift 'notes with spaces.js'` can reopen that file,
while a completed `rm` or a background job is not replayed. Closing an automatically
restored application returns to an interactive shell. Commands run through a
pipeline or redirected away from the terminal are not captured as standalone
foreground applications. Session-management invocations of `mux` are excluded.

**Ctrl-B C** and **Ctrl-B D** set explicit restart-command and directory overrides;
they take priority over automatic capture and persist across restores. An empty
C override requests a shell. **Ctrl-B A** clears both overrides for that pane.
This is also useful for custom shells, compound commands, wrappers, or a job
resumed with `fg`: automatic capture follows the most recent supported launch,
not the shell's job table. Without a valid report, mux retains the pane's startup
recipe. Version 1/2 snapshots preserve their old explicit recipes as overrides.

These are restart recipes, not process checkpoints. Saving does not retain
unsaved editor buffers, process memory, shell variables/environment changes,
background jobs, terminal contents or scrollback text. Save application data
normally. Saving remains explicit; detaching, exiting or shutting down does not
automatically replace the last saved workspace. `mux kill NAME` keeps saved files.
Remove `$HOME/.mux/default` to disable automatic restore of the saved workspace.

Both the snapshot directory (0700) and files (0600) are private to their owner.
The entire layout and restart directories are validated before any command runs.
The shell adds no protocol output outside mux, and ordinary terminal rendering
ignores the private report. Report parsing has bounded memory and does not add
per-character work to the terminal's ASCII fast path.

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
palette. UTF-8 graphemes and wide cells use the shared Unicode engine; complete
xterm emulation and bidirectional layout remain outside this version.

Floating composition uses a back-to-front stack of retained cell surfaces.
Only changed outer cells are sent to the attaching terminal; moving a window
changes geometry and damage, without invalidating/reprinting the whole terminal.
Each window keeps its last complete cell surface so an unfinished synchronized
application update cannot leak through when another window moves or is raised.
Covered windows keep consuming PTY output. A wide grapheme clipped by an
occlusion boundary becomes a blank cell rather than overwriting the window above.
These caches allocate at most one extra content-sized grid per desktop pane.

Session saves write `MUX-SESSION 4`, recording desktop type, preferred
rectangles, window titles, stacking ranks, minimized state and explicit-override flags as well as
restart recipes. The reader still accepts versions 1, 2 and 3. Older mux binaries
cannot read version 4 saves. Validation checks the complete split ownership graph, unique
contiguous stacking ranks and bounded geometry before any saved command runs.
Desktop geometry is in terminal cells; resizing a PTY remains the application's
normal SIGWINCH/TIOCGWINSZ path, without text reflow in mux itself.

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
python3 ports/fuzix/tools/test_mux_floating.py
python3 ports/fuzix/tools/test_mux_snapshot.py
python3 ports/fuzix/tools/test_mux_desktop.py --target both
python3 ports/fuzix/tools/test_mux_windows.py --target both
python3 ports/fuzix/tools/test_mux_default.py --target both
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
