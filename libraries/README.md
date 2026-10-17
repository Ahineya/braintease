# Application libraries

These optional C libraries sit above libc. They are shared by FUZIX and Fuse-X
and have RFL bindings in [`rfl/lib`](../rfl/lib/). Core curses owns cells,
windows, terminal modes, attributes and color pairs. It does not read files or
choose configuration paths, application roles, environment variables or themes.

| Library | C API | RFL API |
| --- | --- | --- |
| Bounded gap buffer, undo and syntax cells | [`text/text.h`](text/text.h) | `lib/text` |
| Ordered INI reader | [`ini/ini.h`](ini/ini.h) | `lib/ini` |
| Semantic theme configuration | [`tui-theme/tui_theme.h`](tui-theme/tui_theme.h) | `lib/tui_theme` |
| Install theme styles into curses | [`tui-theme/tui_curses.h`](tui-theme/tui_curses.h) | `lib/tui_curses` |

INI is independent of filesystems and curses; the theme loader uses libc file
I/O, and the curses installer depends on core curses. All three C APIs work
with the shared C16 guest ABI. INI and theme configuration also compile against
a host libc. There are no Commander-specific role names or paths in these
libraries. Commander supplies those in `rfl/apps/commander/theme_config.rfl`.

## INI

`ini_reader(text, length)` copies the input and returns an owned reader, or NULL
on allocation failure. `ini_next` yields 1 per assignment, 0 at EOF, or
`INI_SYNTAX`/`INI_LIMIT`; getters expose borrowed section/key/value strings and
the one-based line number. Free the reader with `ini_free`. Values remain valid
until the next call or destruction; copy them if they must outlive the reader.

Assignments preserve source order, including duplicates. The format supports
sections, whitespace, whole-line `#`/`;` comments, and matching outer single or
double quotes. It does not interpret escapes or inline comments. Empty values
are allowed. Embedded NUL, malformed syntax, inputs over 64 KiB, lines over 1024
bytes and more than 256 assignments are rejected. Parsing treats text as octets.

## Themes

Create `tui_theme_new(display_name, line_borders)`, then define the application's
roles and defaults with `tui_theme_define(theme, key, fg, bg, flags)`. Roles stay
in declaration order. Palette indices are -1 (terminal default) or 0..255;
flags are `TUI_BOLD`, `TUI_REVERSE`, `TUI_UNDERLINE`. At most 63 roles fit the
shared curses palette. Free all state with `tui_theme_free`.

`tui_theme_named(theme, directory, name)` starts from defaults and loads
`directory/name.ini`, resolving `[theme] base=...` recursively. Named identifiers
contain 1–24 ASCII letters/digits/hyphens/underscores; cycles and chains longer
than eight files fail. `tui_theme_file(theme, directory, path, optional)` layers
a configuration file over the effective theme. Without a base it keeps earlier
layers; with a base it starts from that named theme. Missing optional files are
ignored. Every file load is transactional: errors leave the effective theme
unchanged, and `tui_theme_error` explains the failure. Files must resolve to
regular files and are limited to 16 KiB. Application configuration paths are
supplied explicitly; HOME lookup and system/user precedence are caller policy.

`[theme]` accepts `base`, `name` (up to 127 octets), and `borders=line|ascii`.
`[colors]` accepts only the application's declared keys. Values are foreground,
background and optional `bold`, `reverse`, `underline`, `normal`, separated by
commas or spaces. Colors accept ANSI names, `bright-` names, `default`, or numeric
0..255. Later assignments win. Unknown sections, keys, attributes and colors
fail. `tui_theme_apply` validates one already-parsed assignment; file loaders
resolve bases before applying assignments.

After curses initialization, `tui_curses_install(theme, first_pair, styles,
fallback)` installs color pairs and border mode, filling one `chtype` per role.
The caller allocates `tui_theme_count(theme)` output slots and may supply an
array of fallback attributes for monochrome terminals. The pair range must fit
1..63. This keeps monochrome selection policy explicit in the application.

```c
#include <tui_curses.h>
TuiTheme *theme = tui_theme_new("My app", 1);
chtype styles[2], fallback[2] = {0, A_REVERSE};
tui_theme_define(theme, "body", COLOR_WHITE, COLOR_BLUE, 0);
tui_theme_define(theme, "selection", COLOR_BLACK, COLOR_CYAN, 0);
if (tui_theme_file(theme, "/usr/share/myapp/themes", "/etc/myapp.ini", 1) < 0)
    fprintf(stderr, "%s\n", tui_theme_error(theme));
initscr();
tui_curses_install(theme, 1, styles, fallback);
/* Draw with styles[0] / styles[1], then endwin() and tui_theme_free(theme). */
```

For a Fuse-X C application, `python3 fuse-x/cc.py app.c --tui-theme -o app.fxe`
adds these headers and libraries plus curses. `--ini` adds only INI. For FUZIX,
`make -C ports/fuzix BUILD="$PWD/build/fuzix" config-sdk` builds
`config-ini.pobj`, `config-theme.pobj` and `config-curses.pobj`; link these along
with the existing curses and guest libc objects. The theme sources use relative
header includes, so manually compiling them needs only the ordinary guest SDK.
RFL OS builds detect theme use and link its C implementation automatically;
the INI binding embeds the shared portable C parser directly.

## Validation

`python3 libraries/tests/test_config.py` exercises the C API with a host compiler
(`--cc` can include SDK flags). `cargo test -p rfl --test companion_libraries`
runs the INI binding on standalone Ripple and native targets. The OS integration
runner (`python3 rfl/tools/test_os.py`) exercises the same parser/theme code on
both guests, including installed files and user overrides. The shared curses
probe checks installing a custom C application role on both kernels.

## Text engine

`text/text.h` is independent of curses and filesystems. It provides a gap buffer,
indexed lines, grouped undo/redo, revision-based modified tracking, word/bracket/
quote objects, literal search, indentation and incremental lexical rendering.
`text_render` emits up to 512 packed cells using seven caller-supplied styles;
`ripple_curses_cells` can draw those cells without wrapping. Initial `text_append`
loads without history; call `text_saved` after loading. Normal changes use
`text_begin`, one or more `text_replace` calls, then `text_end` with the final
cursor. Offsets are octets and ranges are half-open; copy/render outputs belong
to the caller. Failed mutations leave the text unchanged. Free with `text_free`.

Files are bounded to 512 KiB/65,536 lines with a shared 2 MiB gap-capacity budget,
plus line indexes and up to 64 groups/1 MiB edit payload per buffer. Raw LF/CRLF
bytes are preserved, NUL is rejected, and printable ASCII is the display target.
The owned RFL wrapper embeds the same C engine for standalone and OS builds.
Rift supplies filesystem/save policy and vi commands; neither is in the engine.
See the [Rift guide](../rfl/apps/rift/README.md) for contracts and extension points.
`python3 libraries/tests/test_text.py` runs the independent randomized/reference
engine checks; `--cc` accepts compiler/SDK flags.
