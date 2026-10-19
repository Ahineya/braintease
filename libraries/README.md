# Application libraries

These optional C libraries sit above libc. They are shared by Riplix and Fuse-X
and have RFL bindings in [`rfl/lib`](../rfl/lib/). Core curses owns cells,
windows, terminal modes, attributes and color pairs. It does not read files or
choose configuration paths, application roles, environment variables or themes.

| Library | C API | RFL API |
| --- | --- | --- |
| Bounded gap buffer, undo and syntax cells | [`text/text.h`](text/text.h) | `lib/text` |
| Ordered INI reader | [`ini/ini.h`](ini/ini.h) | `lib/ini` |
| Semantic theme configuration | [`tui-theme/tui_theme.h`](tui-theme/tui_theme.h) | `lib/tui_theme` |
| Install theme styles into curses | [`tui-theme/tui_curses.h`](tui-theme/tui_curses.h) | `lib/tui_curses` |
| Bounded QR encoding and packed rows | [`qr/qr.h`](qr/qr.h) | `lib/qr` |
| PNG/GIF/JPEG/BMP decoding, RGBA and RGB565 | [`image/image.h`](image/image.h), [`image/nano.h`](image/nano.h) | `lib/image` |
| UTF-8, graphemes, terminal widths and bitmap glyphs | [`unicode/README.md`](unicode/README.md), [`ripple_unicode.h`](../runtime/include/ripple_unicode.h) | `lib/unicode` |

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

Larger application files can use `ini_reader_with_limits(text, length,
max_input, max_line, max_entries)`. The reader enforces each explicit budget;
section storage grows to the smaller of the input and line budgets. Counters
are 32-bit on C16. Existing callers keep the original defaults. RFL exposes
`parse_bytes_with_limits(bytes, Limits { input, line, entries })` in `lib/ini`.
The Kolf course loader uses 256 Ki input cells, 1024 per line and 16384 entries.

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
adds these headers and libraries plus curses. `--ini` adds only INI. For Riplix,
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
bytes are preserved and NUL is rejected. Unicode motion/column/render operations
use the shared Unicode engine, including combining sequences, emoji and wide
continuation cells. `text_boundary`/`text_next`/`text_previous`/`text_step` expose
grapheme boundaries while storage/edit ranges remain octets. Malformed UTF-8 is
displayed safely and retained unchanged. Standalone C builds also link
`unicode/unicode.c`; standalone RFL clients import that C module explicitly.
OS builds use the guest SDK's shared Unicode implementation and glyph pool.
The owned RFL wrapper embeds the same C engine for standalone and OS builds.
`text_wrap_info`/`text_wrap_at`/`text_wrap_column` expose cached visual word
wrapping without editing file contents. Layout computes the requested prefix
and invalidates from the changed logical line. `text_format` optionally inserts
real newlines at whitespace using terminal columns and the existing undo group;
width zero disables formatting.
Rift supplies filesystem/save policy and vi commands; neither is in the engine.
See the [Rift guide](../rfl/apps/rift/README.md) for contracts and extension points.
`python3 libraries/tests/test_text.py` runs the independent randomized/reference
engine checks; `--cc` accepts compiler/SDK flags.

## Images

`image/image.h` is a portable decoder API, independent of the filesystem,
window server and browser. `ri_image_decode(octets, length, limits, &image)`
returns a status and an owned `RiImage`, with straight RGBA in four addressable
octet cells per pixel. Free it with `ri_image_free`. Every failed decode leaves
the output NULL. `ri_image_error` describes the error; `ri_image_new` creates
a zeroed canvas. There is no global mutable decoder state or retained input.
On C16, an octet occupies one 16-bit cell and values above 255 are rejected.

Supported formats:

- PNG: grayscale, RGB, indexed, grayscale-alpha and RGBA; valid 1/2/4/8/16-bit
  sample depths, all five row filters, Adam7, palettes and `tRNS`. CRC, zlib
  header, exact DEFLATE output and Adler checks reject malformed streams.
  Sixteen-bit samples reduce to their high octet.
- GIF87a/89a: palettes, transparency, offsets, interlacing and bounded LZW.
  Only the first displayed frame is returned. Animation/disposal timing and
  plaintext graphics are not implemented.
- Baseline JPEG: grayscale and YCbCr 4:4:4, 4:2:2, 4:4:0 and 4:2:0, including
  restart intervals. Progressive/multiscan, CMYK, RGB JPEG and arithmetic
  coding are unsupported. Chroma uses nearest-neighbour reconstruction.
- BMP: OS/2 or Windows headers, uncompressed 1/4/8-bit palettes and 24/32-bit
  pixels, row padding and top-down Windows images. BI_RGB's unused fourth
  channel is ignored. RLE, bitfields and BMP alpha masks are unsupported.

Defaults bound each dimension to 512, decoded pixels to 65,536, input to
262,144 octets and temporary work to 786,432 C cells. `RiLimits` allows explicit
smaller or larger bounds, within the hard 4096-dimension/1,048,576-pixel and
input ceilings. Work counts temporary allocations, separately from the owned
RGBA canvas. Applications must impose aggregate cache budgets themselves.
There is no ICC/gamma correction, EXIF orientation, SVG, WebP, ICO or animation.

Compile `libraries/image/decode.c` once, with the ordinary C16 SDK, or compile
`image.c`, `png.c`, `gif.c`, `bmp.c`, `jpeg.c` and `../deflate/deflate.c` separately.
Do not link both forms. The implementation needs only libc. `adapt.py` verifies
the pinned, unmodified public-domain picojpeg import and reproduces its private
generated adapter. The adapter uses per-decoder state, widened arithmetic and
checked markers/tables, and fixes the imported SOS-header consumption bug.
`vendor/source.json` records provenance and hashes. New image code is MIT;
the shared DEFLATE extraction retains Joergen Ibsen's zlib license and its
original notice. It derives from the existing C16 Telegram inflater.

PNG decoding selects a predictor once per scanline, skips the no-filter pass,
and uses contiguous 8-bit RGB/RGBA paths before falling back to the complete
sample-depth/Adam7/transparency implementation. The shared inflater keeps its
Huffman bit reader in scalar locals for a complete symbol, refilling only when
needed; it does not prefetch across stored-block boundaries. These are portable
guest algorithms, with the same CRC/Adler, truncation and allocation checks.
`python3 libraries/tests/test_deflate.py` checks 41 independently generated zlib
streams, including fixed/dynamic/stored blocks, sync-flush transitions, 66,000
octet output, truncated/trailing input and undersized output buffers under UBSan.
`libraries/tests/image_bench.c`, linked with `libraries/image/decode.c`, accepts
an image path and iteration count to measure decoding inside either guest.

`image/nano.h` and the separate `nano.c` adapter provide `ri_nano_draw` for
Nano-X applications: scaled and repeated drawing into an explicit clipped
rectangle, preserving the GC's additional clipping. It converts to RGB565 and
uploads at most 128-by-32 tiles. Client and server staging each need at most
8192 C cells; resources are released after each tile. The current FuseDE image
protocol's alpha plane blends against actual destination pixels, including
partial transparency. It retains the existing per-client, global and memory
quotas. This adapter links the ordinary Nano-X/FuseDE client library and needs
an updated server. The decoder itself has no graphics dependency.

`ri_nano_draw_cached` is an optional retained-resource path. Its caller must
clip the GC to the supplied rectangle, call `ri_nano_forget(image)` **before**
changing or freeing RGBA, and `ri_nano_clear()` before closing the connection.
It keeps at most three server resources/65,536 cells, expands small repeated
patterns once, and falls back to ordinary visible tiles when a source exceeds
its bounds or resources are unavailable. No decoder ownership rules change.
An independent pixel oracle and protocol-operation counter run with
`python3 libraries/tests/test_image_nano.py`; hosted clips reach 1920x1080.

The owned RFL wrapper embeds exactly the same C engine:

```rfl
import { decode } from "lib/image";
import {} from "std/result";
import {} from "std/option";
let image = decode(bytes)?;
let width = image.width();
let pixel = image.rgba(0, 0); // Option<u32>, packed 0xRRGGBBAA
```

`Image` automatically frees its canvas; `width`, `height`, `opaque` and checked
`rgba` accessors are available without a renderer. For bulk drawing C clients
should pass the canvas to the Nano-X adapter rather than query every pixel.

`python3 libraries/tests/test_image.py --target host` runs independent format
vectors, pixel hashes, JPEG comparison, bank-sized canvases, truncation,
allocation failures, guarded buffers and mutated inputs under UBSan. The fixture
generator needs `cjpeg`/`djpeg` (libjpeg). `--target both` additionally compiles
and runs the same checks inside disposable Fuse-X and Riplix/RVM kernels, then
checks their filesystems. `--compiler` chooses RCC. The RFL wrapper is covered
by `cargo test --release -p rfl --test image_library`. Actual graphics, clipping,
scrolling and HTTP/HTTPS loading are covered by NetSurf's `tests/images.py`.
