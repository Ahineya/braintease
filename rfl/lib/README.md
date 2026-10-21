# RFL companion libraries

Use `lib/<name>` for optional, bundled application libraries. `std/` contains
core language/runtime and OS service bindings; companion libraries implement
reusable policies and formats above those services. Application-private code
continues to use relative imports such as `./model`. This is a bundled namespace,
not a package manager: imports resolve inside this checkout's `rfl/lib`, using
the same resolver for compilation, LSP navigation and documentation hover.
A `.rfl` suffix is optional. Parent-directory components in library imports are rejected.

Current modules:

- [`lib/music`](music.rfl): owned guest WAV player with a C streaming engine,
  pause/resume, relative seeking, volume and playback status. Built for both
  Unix targets; see [music usage and device details](../../ports/music/README.md).

- [`lib/qr`](qr.rfl): owned C QR encoder, byte payloads up to 84 octets,
  medium error correction, complete quiet zone and two module rows per curses
  cell. At most 45 columns by 23 terminal rows; generation needs no OS services.

- [`lib/tui`](tui.rfl): `Rect` and a statically dispatched `Canvas` drawing
  contract implemented for curses `Screen` and `Window`.
- [`lib/tui_list`](tui_list.rfl): item-independent `Selection`, with clamped
  movement, paging and viewport repair after list changes/resizes.
- [`lib/tui_input`](tui_input.rfl): `LineEdit`, shared by prompts and file
  dialogs; cursor movement, editing and horizontal scrolling.
- [`lib/tui_files`](tui_files.rfl): Commander's extracted `FilePanel` model and
  renderer, directory-first sorting, refresh, hidden files and prefix search.
- [`lib/tui_file_dialog`](tui_file_dialog.rfl): an Open dialog composed from
  `FilePanel`, `LineEdit` and owned popup layers. Used by Rift.

- [`lib/text`](text.rfl): owned C gap buffer, indexed lines, grouped undo/redo,
  search/text objects and incremental syntax cells. Independent of curses and
  filesystem policy; works with standalone and both OS targets.
- [`lib/text_curses`](text_curses.rfl): optional direct rendering from a text
  buffer into curses, without a temporary RFL cell vector. OS builds only.
- [`lib/tui_dialog`](tui_dialog.rfl): centered editable prompts, scrollable menus
  and messages with caller-supplied palettes and resize handling.
- [`lib/path`](path.rfl): lexical absolute guest paths, join/parent/basename;
  does not resolve symbolic links.

- [`lib/ini`](ini.rfl): ordered, bounded INI parsing using the shared portable
  C parser. Works with standalone `rfl run` and both OS builds.
- [`lib/tui_theme`](tui_theme.rfl): owned theme loader, role/value snapshots,
  named-theme inheritance and configuration layers. The application supplies
  role names, defaults, directories, paths and layer order. Uses guest libc
  through `rfl build --os fuzix|fuse-x`.
- [`lib/tui_curses`](tui_curses.rfl): installs semantic roles as curses styles,
  with caller-supplied monochrome fallbacks and a validated color-pair range.

`std/ini` remains a compatibility adapter over `lib/ini`; new code should import
`lib/ini`. INI syntax, theme format, limits, C APIs and build instructions are
in [`libraries/README.md`](../../libraries/README.md). Curses itself does not
parse configuration or know the application's role names.

```rfl
import { loader, Config, Role } from "lib/tui_theme";
import { install } from "lib/tui_curses";
import { REVERSE } from "std/curses";

// In a function with an active Screen:
let body = Role { foreground: 7, background: 4, bold: false, reverse: false, underline: false };
let selected = Role { foreground: 0, background: 6, bold: false, reverse: false, underline: false };
let defaults = Config { name: "My app", roles: [body, selected], lines: true };
let theme = loader(defaults, ["body", "selection"])?;
theme.file("/usr/share/myapp/themes", "/etc/myapp.ini", true)?;
// Apply an optional user file next, using a HOME-derived path selected by the app.
let styles = install(screen, theme.config(), 1, [0, REVERSE])?;
screen.text(0, 0, "Selected item", styles[1]);
```

A failed load preserves the loader's current values; `config()` returns an
independent value snapshot. The RFL APIs use `Result`, owned cleanup and `///`
documentation, so IntelliJ can navigate imports and show help through the LSP.

## Composing TUI widgets

Widget state, drawing and the application event loop are separate. `Selection`
and `LineEdit` retain their state across repaints. `FilePanel` owns a directory
snapshot and a selection/viewport; movement does not read the directory again.
The Open dialog retains its file rows while editing the filename and does not
redraw on input timeouts.
Its `refresh`/`enter` methods commit a new listing only on success. The generic
renderer borrows any `Canvas`: Commander draws into `Screen`, while Rift's
picker draws into a floating `Window`. It allocates no windows and does not
present, block on keys, change the working directory or choose a global palette.

Pass a local `Rect` and semantic `PanelPalette`, then compose with the existing
`Screen.stage`, `Window.stage`, `Screen.present` boundary. `visible_rows(area)`
provides the viewport height; call `panel.reveal(rows)` after resize or changing
a selection. `panel.key(key, rows)` reports whether it consumed a standard list
navigation key; applications retain Enter, shortcuts, focus and file-operation
policy. `Canvas` uses static trait dispatch, so this API requires neither a
runtime widget tree nor per-cell dynamic dispatch.

```rfl
import { Rect } from "lib/tui";
import { file_panel, draw, visible_rows, PanelPalette } from "lib/tui_files";

// Once, with an active curses session and an application-supplied palette:
let mut panel = file_panel("/", false)?;
let area = Rect { row: 1, column: 0, rows: 20, columns: 40 };
let rows = visible_rows(area);
panel.reveal(rows);
draw(screen, &panel, area, colors, true, true);
// In the application's event loop: panel.key(key, rows).
```

For a complete Open dialog:

```rfl
import { open_file, OpenOptions } from "lib/tui_file_dialog";

let options = OpenOptions {
    directory: "/", selected_name: "", allow_new: false, show_hidden: false, lines: true
};
match open_file(screen, palette, options)? {
    Some(path) => { /* application opens this absolute guest path */ },
    None => { /* cancelled; existing application state is intact */ }
}
```

The filename starts empty and focused, so callers can type an absolute or
relative path immediately. Tab switches between filename and list. Enter opens
an existing regular file or enters a directory; Right enters directories only.
Arrows, Home/End and Page Up/Down navigate. Typing in the list searches an ASCII
prefix; `/` returns to absolute-path entry. Ctrl-B goes to the parent and
reselects the directory just left, Ctrl-T toggles hidden entries, and Ctrl-R
refreshes the listing. Esc/Ctrl-C cancels. Directory, selection, filename and
editing cursor survive resizing, including a temporarily undersized terminal.

`allow_new` permits a typed nonexistent path to be returned without creating it;
the caller owns creation and overwrite policy. Symlinks and special files are
not accepted by this first picker. Listings retain the existing 4096-entry
POSIX bound, input is limited to 255 ASCII characters, and directory order is
bytewise with directories first. This is a bundled application library under
`lib/`, with `///` hover documentation, not a new standard-library or packaging
mechanism. Save As browsing, multi-selection and tree views can be
added above the same state/drawing contracts.

The shared directory panel already exposes `FilePanel.mouse(area, event)` for
click selection and wheel navigation, using the same rectangle as `draw` or
`draw_cached`. Enable a mode such as `screen.mouse(MOUSE_DRAG)` and read
`screen.event().mouse`. `Rect.contains` hit tests zero-based terminal cells.
Commander also demonstrates divider dragging and function-button clicks.
Keyboard controls remain available; Shift-drag selects terminal text instead of
sending application mouse events. See the [mouse architecture](../../docs/graphics/architecture.md#terminal-mouse-interaction).

## Retained panels and reusable I/O

`lib/tui_files` also provides `panel_paint()` and `draw_cached(...)`. Keep one
`PanelPaint` per surface/rectangle, and set `valid = false` after an overlay or
other drawing replaces its cells. Geometry, palette, focus, viewport and listing
changes trigger a full panel repaint; selection-only movement redraws the old/new
rows, count and scrollbar. `FilePanel.refresh` and `enter` advance `generation`.
Code that directly replaces entries must also advance that token. The original
`draw` remains an unconditional renderer. `Canvas.glyphs` batches a repeated glyph
run into one drawing call.

`std/posix.read_into` / `File.read_into` fill an initialized reusable `Vec<u8>`
and return its valid prefix length without changing its size. `write_prefix`
writes that prefix; `lib/text.hash_prefix`, `Buffer.append_prefix` and
`Buffer.copy_into` consume or produce ranges without temporary slices. Initialize
fixed buffers once, keep the returned length, and never treat an unused tail as
file data. The allocating `read` and `write_all` APIs remain available.

`Buffer.damage()` consumes all accumulated edit damage, including syntax
propagation, before painting a frame. It has one rendering consumer per buffer;
newlines invalidate the suffix, and untouched uncached syntax stays lazy.
`lib/text_curses.paint_line` returns -1 on error, 0 for a fitting line or 1 for
right overflow. The boolean `paint` wrapper remains available.

Mouse-aware lists and fields use `Selection.mouse(Rect, MouseEvent, count)` and
`LineEdit.mouse(Rect, MouseEvent)`. Supply rectangles in the same coordinate
space as the event. `Popup.mouse(event)` converts screen-relative reports to
local coordinates. Shared choice, prompt, message and file-picker dialogs now
support mouse interaction when their application enables reporting. A list click
selects; the caller decides activation. Choice menus activate on click, while
the file picker opens a selected entry when clicked again. Field clicks account
for horizontal scrolling, combining clusters and wide glyphs.
