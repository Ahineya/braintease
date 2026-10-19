# Shared C16 curses

The screen library is built from the curses sources in the pinned FUZIX archive
plus `ripple/curses/`. It runs inside the guest on both RVM and native Fuse-X;
it does not link host ncurses. Include `ripple/include/curses.h` through the
normal userspace include paths.

Only consumers link curses. For native programs:

```sh
python3 fuse-x/cc.py --curses program.c -o /tmp/program.fxe
```

For RVM, link `CURSES_OBJS` from the Riplix Makefile alongside ordinary userspace
objects. `cursescheck.rxe` is an example target; the library/probe is not installed
on normal disks automatically.

## Behavior and limits

- `keypad` decodes ANSI CSI and SS3 arrows, Home/End, Insert/Delete, Page Up/Down,
  back-tab, SS3 F1–F4 and numeric CSI F1–F12. Unknown/incomplete sequences return their original
  characters, starting with Escape. A possible key sequence has a bounded
  150 ms ambiguity wait, including when nodelay is enabled. An empty nodelay
  read returns immediately.
- `wtimeout`, `timeout`, `nodelay` and `halfdelay` use guest `select`, not busy
  polling or an application alarm. Signals interrupt input with `ERR/EINTR`;
  hangup returns `ERR`. FILE lookahead and `ungetch` are consumed first.
- Use `wgetnstr(window, buffer, capacity - 1)` for bounded line input. The legacy
  `wgetstr` cannot know a buffer's capacity and returns `ERR/EINVAL`. The scanw
  wrappers use their own bounded 512-character input.
- Screen cells are 32-bit `chtype` (`unsigned long`, two C16 characters).
  Bits 0–20 hold a Unicode scalar, interned grapheme identifier or wide-cell
  continuation. Bits 21–25 hold blink, bold, reverse/standout, underline and
  DEC graphics; bits 26–31 hold one of 64 color-pair identifiers.
  `WINDOW._line`, `WINDOW._attrs`, `waddch` and `winch` preserve the full cell.
  Rebuild applications and their curses objects together after this ABI change.
  Attributes are never emitted as text. Use the macros instead of numeric masks.
  Standout and reverse share a bit. This is a Unicode-capable curses subset,
  rather than a complete implementation of ncurses' wide-character API.
- `waddstr`/`wadd_utf8` decode UTF-8; `waddch` accepts a Unicode scalar plus
  attributes. Combining marks extend the preceding grapheme, wide characters
  occupy a head and continuation cell, and refresh emits the original UTF-8.
  Malformed input renders replacement characters. Clipping never emits a
  partial UTF-8 encoding. The bounded display pool and width/font policy are
  documented in [the shared Unicode library](../../libraries/unicode/README.md).
- `wget_wch(window, &value)` returns `OK` for a Unicode scalar,
  `KEY_CODE_YES` for a function key, or `ERR` for timeout/error. Always inspect
  the return status: some Unicode scalar values equal legacy `KEY_*` numbers.
  Its decoder survives fragmented reads, timeouts and resize. Legacy `wgetch`
  still returns raw bytes and function keys for existing applications.
  `wgetnstr` edits and erases complete UTF-8 graphemes; its limit counts octets.
  RFL exposes the distinction through `Screen.event()`/`Window.event()`;
  `.key()` remains appropriate for ASCII shortcuts. Reusable `lib/tui_input`
  fields use `.event()` and report cursor positions in display columns.
- `ripple_curses_bracketed_paste(1)` requests host bracketed paste. Input reports
  `KEY_PASTE_BEGIN`/`KEY_PASTE_END`; bytes between them remain literal, including
  escape sequences. Split end markers survive polling timeouts. Suspend/exit
  disables the terminal mode, and resume restores it. RFL exposes the request
  as `Screen.bracketed_paste(true)`.
  `ripple_curses_text_run` collects complete, ready UTF-8 scalars into a bounded
  caller buffer without refreshing per character. Nonliteral runs stop before
  function/control events, retaining them for the ordinary event reader. Chunk
  lookahead uses stdin's existing FILE buffer so other stdio consumers share it.
- ANSI SGR rendering implements bold, reverse/standout, underline and blink
  without relying on optional `mr`/`so` entries in an existing termcap file.
  This fixes invisible selection when those capabilities are absent. Attribute
  state is invalidated across suspend/resume and full redraws.
- `has_colors`, `start_color`, `use_default_colors`, `init_pair`, `pair_content`,
  `COLOR_PAIR` and `PAIR_NUMBER` support 63 configurable pairs plus default pair
  zero. Foreground/background accept -1 (terminal defaults) and 0–255. Indices
  0–15 use basic/bright ANSI sequences; higher indices require a 256-color
  terminal. `NO_COLOR` and `TERM=dumb` disable initialization. Redefining a pair
  invalidates the physical screen so existing cells repaint in the new colors.
- `ripple_curses_lines(1)` selects DEC Special Graphics borders; zero selects
  ASCII. Both the host terminal and the guest graphics terminal can display the
  line glyphs (the latter uses its Unicode bitmap font). Unknown termcap names
  fall back to the installed ANSI entry.
- Independent windows compose from back to front through `wnoutrefresh` and
  `doupdate`. Staging copies each complete layer, so overlapping unchanged
  windows repaint correctly when a lower layer changes. Stage the background
  again after moving/deleting a layer to reveal its previous area. `wbkgd`
  fills a window with styled spaces; it is the port's fill/clear operation,
  rather than ncurses' character-rewriting behavior. Signed `wscrl` shifts an
  inclusive scrolling region and fills exposed rows using the current style.
  It preserves parent aliases when scrolling a derived window.
- Valid dimensions are 1–256 rows and 1–512 columns. Allocation failures return
  NULL/ERR. Derived windows retain parent rows; deleting a parent with live
  children returns `ERR/EBUSY`. Moving an aliased child or a parent with children
  to another origin is rejected.
- Independent windows retained across a resize keep their dimensions, but
  staging clips them to the new screen bounds rather than writing outside the
  destination. Applications can recreate or reposition floating dialogs after
  `KEY_RESIZE`.
- Input checks terminal dimensions and reports `KEY_RESIZE` after a successful
  resize. `resizeterm` allocates replacement buffers transactionally and retains
  the `stdscr`, `curscr` and pending-screen WINDOW identities. Text is clipped to
  the new dimensions. Resize while these screens have derived windows returns
  `ERR/EBUSY`; delete/recreate those children explicitly.
- Physical-console dimensions are sampled from the live host through device
  ABI 1 `HOST_TERMINAL`, then exposed by `TIOCGWINSZ`. An unavailable host retains
  the cached/default size. PTY dimensions continue to come from their graphics
  or Telnet clients. Fuse-X also forwards the attached client's dimensions into
  the broker PTY, so normal persistent sessions resize alongside direct runs.
- `endwin` is repeatable, restores shell termios, attributes and cursor, and
  retains windows. `reset_prog_mode` or the next refresh/input resumes terminal
  mode and forces a redraw. Normal `exit` runs a registered restoration hook.
  The library does not install signal handlers: applications must arrange
  `endwin` before stopping and `reset_prog_mode` after continuation, and must
  clean up their own signal/error exits.

The accompanying C16 libc additions provide integer/string `sscanf`/`vsscanf`,
`usleep` through select, and anonymous `tmpfile`. Scanning supports integer
conversions, `%p`, `%c`, `%s`, scansets, `%n`, suppression, widths and C16 short/
long/size modifiers. Floating, wide-character and long-long scanning return
`EINVAL`; numeric overflow returns `ERANGE` without storing the overflowing
value. This is a documented subset, not a complete ISO scanf implementation.

`ripple_curses_cells(window, row, column, cells, count)` writes packed styled
cells as a clipped row without wrapping or scrolling, including at the lower
right corner. It preserves each cell's attributes and DEC glyph flag, sanitizes
non-printable ordinary glyphs, marks changed columns and returns OK/ERR.
`ripple_curses_cursor_shape(0..3)` requests terminal default, steady block,
underline or bar via DECSCUSR; `endwin` restores the default shape. Unsupported
terminals may ignore shape requests. RFL exposes these as `Screen.cells` and
`Screen.cursor_shape`; Rift uses them for highlighting and vi mode indication.

## Validation

From the repository root, with the corresponding kernel/apps already built:

```sh
python3 ports/fuzix/tools/test_curses.py --native
python3 ports/fuzix/tools/test_curses.py --build build/fuzix
```

Both tests boot an actual kernel with a disposable disk and exercise a private
guest PTY. The expanded probe includes 115 assertions on native Fuse-X and
RVM JIT: scanner boundaries, temporary file I/O, raw/echo modes, key decoding,
FILE/ungetch lookahead, bounded editing, timeout/halfdelay, fragmented escape
input, signal interruption, window ownership, attribute output, window dimensions, resize, repeated
endwin, actual SIGSTOP/SIGCONT, atexit restoration, PTY hangup, full-width color
cells, pair validation, line glyphs, composed/moving windows and bidirectional
scrolling (including parent aliases), batched per-cell attributes/clipping,
no lower-right scrolling and cursor-shape validation. Unicode checks cover
scalar/style preservation, wide-cell overwrite, combining marks, fragmented
input, text/function-key collisions and grapheme-aware bounded input.

`python3 rfl/tools/test_unicode.py --os both` also checks the RFL widgets,
Unicode shell arguments/editing, scratch-buffer boundaries and VT throughput
on disposable guest disks. `python3 ports/graphics/tests/test_unicode_render.py`
checks actual Cyrillic, Greek and combining-accent pixels in gterm on both
guests and nxterm on Fuse-X.

The RFL [OS integration suite](../../rfl/os/README.md#validation) additionally
checks the visible selection colors, INI theme inheritance/overrides, floating
dialogs, theme switching, monochrome fallback and directory paging through
actual console input on both platforms.

This establishes the shared terminal prerequisite. It does not establish that
a complete browser links, fits memory, or renders correctly.

## Alternate screen and companion libraries

`initscr` enters the ANSI alternate screen (`?1049h`). `endwin` resets styles,
character set and cursor, restores the normal screen (`?1049l`) and restores
shell input modes; repeated calls do not switch screens again. `reset_prog_mode`
reenters the alternate screen only when suspended and invalidates the rendered
cache for a complete redraw. This applies to normal exits, the libc exit hook,
editor handoff and RFL failures. Window storage survives suspend/resume.

Curses itself does not load configuration files. Optional INI parsing and
semantic theme loading live in [`../../libraries`](../../libraries/README.md),
with `tui_curses_install` for C and `lib/tui_curses` for RFL. Applications declare
roles, defaults, monochrome fallback attributes and configuration paths.
The C probe includes a custom role install and invalid pair-range checks.
