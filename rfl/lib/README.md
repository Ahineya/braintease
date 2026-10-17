# RFL companion libraries

Use `lib/<name>` for optional, bundled application libraries. `std/` contains
core language/runtime and OS service bindings; companion libraries implement
reusable policies and formats above those services. Application-private code
continues to use relative imports such as `./model`. This is a bundled namespace,
not a package manager: imports resolve inside this checkout's `rfl/lib`, using
the same resolver for compilation, LSP navigation and documentation hover.
A `.rfl` suffix is optional. Parent-directory components in library imports are rejected.

Current modules:

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
