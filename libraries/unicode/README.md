# Shared terminal Unicode

Riplix/RVM and Fuse-X use the same Unicode 16.0.0 implementation for UTF-8,
extended grapheme boundaries and terminal widths. Terminal text stays UTF-8
through files, pipes, PTYs and application strings. A C16 character holding
UTF-8 contains one octet; it is not UTF-16. Lengths and string cursor offsets
remain octet counts. Display columns are a separate quantity.

## APIs

The C interface is [`runtime/include/ripple_unicode.h`](../../runtime/include/ripple_unicode.h).
Link `unicode.c`; graphical renderers additionally link `font.c`.

- Initialize `struct ru_decoder` to zero and feed octets with `ru_decode`.
  A result of zero means incomplete input; one produces a scalar or U+FFFD;
  minus one produces U+FFFD and requires retrying the same octet. Retain the
  decoder across reads. At final EOF, a nonzero `need` means one incomplete
  sequence remains. `ru_encode` writes up to four octets without allocation.
- `ru_next` decodes from a bounded octet span and advances a byte offset.
  It replaces malformed input and never consumes a valid following lead octet.
- `ru_next_grapheme`, `ru_previous_grapheme`, `ru_prefix` and `ru_columns`
  provide extended grapheme movement, whole-grapheme clipping and widths.
  Start positions should be grapheme boundaries; lengths and positions use
  `size_t` (32 bits in the guest ABI).
- `ru_join` supports streaming grapheme boundaries without allocating strings.
  Its state implements Unicode 16 extended grapheme rules, including regional
  indicator pairs, emoji ZWJ sequences and Indic conjunct boundaries.
- `ru_append`, `ru_scalar_count`, `ru_scalar` and `ru_glyph_width` provide
  process-local, stable display identifiers for multi-scalar graphemes.

RFL's `lib/unicode` exports `columns`, `prefix`, `next` and `previous`.
ASCII uses a direct path; non-ASCII strings cross the C boundary as bounded
octet vectors. OS builds detect the dependency and link the shared engine.
Standalone applications can explicitly import `libraries/unicode/unicode.c`,
as demonstrated by `rfl/tests/fixtures/tui_widgets.rfl`.

`lib/tui_input` moves and erases complete graphemes, accepts scalar input, and
provides the display-column cursor position. Reusable dialogs and file fields
use this interface. See [curses](../../ports/fuzix/CURSES.md) for wide events and
the packed styled-cell ABI. Existing byte-oriented `wgetch` callers continue
to receive octets; migrating an application to Unicode editing is explicit.

## Terminal model and rendering

Each VT cell still occupies 32 bits (two C16 words): 21 glyph bits and 11 style
bits. Wide glyphs have one head and one continuation cell. The model repairs
split wide cells after erase, overwrite, column insertion/deletion and resize.
If a streamed grapheme grows from one to two columns only after its head was
drawn in the final column, the VT uses a replacement glyph. Clipped RFL drawing
omits that whole grapheme instead. Ordinary wide scalars wrap normally.
Both screen planes, dirty rows, scrolling copies and ASCII rendering runs
remain in use. The multi-scalar pool is allocated only on first use, holds
512 distinct graphemes of up to 16 scalars, and never evicts live identifiers.
Repeated graphemes reuse their entry. Overlong clusters, allocation failure
or an exhausted pool produce U+FFFD for display; application text is retained.

Widths use East Asian Wide/Fullwidth = 2, Ambiguous = 1, combining/format
characters = 0, ordinary characters = 1. Emoji presentation, keycaps and
regional indicator pairs occupy two columns. This is a fixed terminal policy;
an external terminal with a different width policy can disagree. The library
does not normalize text or change original encodings. Controls are handled by
the terminal/application, rather than counted as printable columns.

gterm and nxterm retain ROM ASCII drawing and use a generated bitmap subset
for Latin, Greek, Cyrillic, combining marks, punctuation, arrows and box
drawing. Marks overlay the base glyph. The 2,256 glyphs are 16 pixels tall;
lookup uses a small direct-mapped cache, with no font parsing or allocation
during painting. Missing glyphs show a replacement and preserve their logical
width. The font is compiled into graphical terminal clients, not every TUI.

RVM and Fuse-X graphical input carry committed Unicode scalars separately
from physical keys. Printable physical keys retain held-key state but are
marked so terminals avoid typing twice; Ctrl shortcuts keep the key path.
Host character events and IME commits become UTF-8 on the guest PTY. Nano-X
transports the resulting octets through its existing keyboard event carrier.

Telegram preserves incoming UTF-8, wraps history at grapheme/cell boundaries,
and accepts Unicode composition, search and password input. Password scalars
are encoded directly into its erasable C buffer. The shell keeps all eight
octet bits through quoting and performs grapheme-aware interactive movement
and deletion. Terminal mux display and copied selections preserve UTF-8.

This does not make every historical application Unicode-aware. Kernel
canonical erase, legacy editors/text engines, mux command/search fields and
other byte-based application logic still need individual migration. There is
no locale/wchar compatibility layer, complex script shaping, bidi layout,
full CJK bitmap repertoire or color emoji rendering. Grapheme grouping and
widths already preserve space for those future renderers.

## Build and validate

Normal launch scripts rebuild affected components and install managed apps
while preserving user files. Exit any running guest before updating its disk:

```sh
scripts/run-fuzix.sh --build-only
scripts/run-fuse-x.sh --build-only
```

Use normal launches afterward; `--run-only` intentionally reuses old binaries.
Rebuild curses consumers and their library together because attribute bit
positions changed. On macOS, if the selected SDK is newer than the installed
linker, set `SDKROOT` to a compatible installed SDK before building.

Host and guest checks, from the repository root:

```sh
python3 libraries/tests/test_unicode.py
python3 ports/graphics/tests/run.py
cargo test -p rfl --test unicode_literals --test tui_widgets --test string_literals
python3 ports/telegram/tests/test_model.py
python3 libraries/tests/test_qr.py --target host
python3 rfl/tools/test_unicode.py --os both
python3 ports/graphics/tests/test_unicode_render.py --os both
```

Guest tests require the normal kernels, boot images and echo programs already
built. They build probes and private SDKs under `build/unicode`, use disposable
roots, and never install into the user's working disk. The acceptance test
runs 115 curses assertions, quoted shell octets, interactive grapheme editing,
split input, a drawing-buffer boundary and wide clipping on both kernels.
The pixel test checks actual Cyrillic, Greek and combining-accent bitmaps in
gterm on both guests and nxterm on Fuse-X. Host conformance checks cover all
1,093 vectors in the pinned Unicode 16 grapheme suite.

For a before/after VT benchmark, retain the original `terminal.c` and its
matching `terminal.h` in one directory, then add
`--baseline /absolute/path/to/terminal.c` to `test_unicode.py`. It warms both
executables, alternates their order, and records five samples and medians in
`build/unicode/acceptance/results.json`. Each sample feeds 2,000 ASCII lines
and 2,000 mixed-script lines with scrolling. This measures VT parsing/cell
updates, excluding pixel painting, transport and end-to-end typing latency.
The old implementation did not render the mixed stream correctly.

Measured on this macOS development machine on 2026-10-18 (milliseconds,
five-sample medians, RVM JIT enabled):

| Guest | Original ASCII | Unicode ASCII | Original mixed octets | Unicode mixed text |
| --- | ---: | ---: | ---: | ---: |
| Fuse-X | 1,556 | 948 | 1,335 | 1,892 |
| Riplix/RVM | 4,049 | 3,567 | 3,726 | 4,785 |

ASCII median time fell about 39% on Fuse-X and 12% on RVM after batching
printable runs. Correct mixed-script processing added about 42% and 28%
respectively to this workload. Host scheduling and JIT warmup cause substantial
variation, particularly on RVM; these are workload measurements, not latency
guarantees. Cell storage stayed two C16 words in both builds. Multi-scalar
display storage and the graphical font are additional bounded costs.

## Data and font provenance

Normal builds are offline. Generated `tables.inc`, `font.inc` and the official
grapheme test fixture are committed. `generate.py` verifies every fetched input
against the URLs and SHA-256 digests in `sources.json`; cached inputs live in
`build/unicode/vendor`. Run it only when intentionally regenerating data.

Unicode property data and tests retain [UNICODE-LICENSE.txt](UNICODE-LICENSE.txt).
The Ripple terminal bitmap subset derives from
[GNU Unifont 16.0.04](https://unifoundry.com/pub/unifont/unifont-16.0.04/)
and is distributed under OFL 1.1; see [FONT-COPYRIGHT.txt](FONT-COPYRIGHT.txt)
and [FONT-LICENSE.txt](FONT-LICENSE.txt). The independent C implementation and
font lookup wrapper are MIT licensed. Installed systems include these data
and font notices under `/usr/share/licenses/unicode/`.
