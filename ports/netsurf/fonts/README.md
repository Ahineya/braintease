# Ripple Web bitmap subset

These renamed bitmap strikes derive from the official Liberation Fonts 2.1.5
release, under the SIL Open Font License 1.1. Copyright, license and author
notices are retained in `LICENSE` and `AUTHORS` and installed with the browser.
`source.json` records the archive URL, SHA-256, original face hashes, generator
versions and output hash. Original upstream source:
<https://github.com/liberationfonts/liberation-fonts>.

Normal builds are offline and install the checked-in `web.nfa`; no Python
imaging library or TrueType font is needed to build or run the browser.
To regenerate, use Pillow 12.3.0 / FreeType 2.14.3 and the pinned archive:

```sh
python3 ports/netsurf/fonts/generate.py /path/to/liberation-fonts-ttf-2.1.5.tar.gz
```

The archive hash is checked before extraction. The generator renders twelve
faces (sans, serif, mono × regular, bold, italic, bold-italic), at 16 and 32
pixels, and quantizes coverage to four bits with run-length encoding.
`compose.inc` contains canonical composition pairs for the subset.

The font file uses serialized octets, independent of the host/C16 ABI:

- 20-octet header: `RBFONT01`, little-endian u16 face/strike/codepoint counts,
  zero u16 reserved, u32 total length.
- Sorted u16 code points, followed by face/strike/codepoint-major records.
- 12-octet records: u32 data offset, u16 compressed length, signed octet left
  and top bearings from the baseline, octet width/height/advance, zero reserved.
  Offset `0xffffffff` selects the shared Unicode bitmap fallback.
- RLE coverage: high nibble is run length minus one, low nibble is coverage
  (multiply by 17). A glyph expands to exactly width × height samples.

The reader caps the file at 4 MiB, code points at 2,048 and source glyphs at
64×64. It checks sorted keys, offsets, compressed extents and expanded lengths
before exposing glyphs to the painter. Strikes are resampled into bounded
client atlases; the existing Nano-X image protocol tints and alpha blends them.

`tests/fonts.c` checks proportional/monospace advances, scaling, canonical
clusters, UTF-8 hit testing, wrapping, all faces/sizes, and corrupt font data.
It can run on the conventional host or as an ordinary guest application.
`tests/fonts_paint.c` exercises the real atlas builder/resampler with guarded
allocations and strict drawing stubs. `python3 ports/netsurf/tests/fonts.py`
runs both host checks. The live `tests/fonts_gui.py` uses a disposable Fuse-X
image for antialiasing over dark/light backgrounds, resizing and link clicks.
`tests/text_gui.py` compares real Cyrillic pixels against the stored strike.
