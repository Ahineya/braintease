# FuseDE artwork

**Slate** is the first original FuseDE theme: charcoal surfaces, satin bevels,
inset fields and a muted blue accent. Its editable source is `slate.json` plus
the pixel-aligned drawing rules in `../tools/build_theme.py`. It is created for
this project, not extracted from macOS, a commercial skin or a downloaded pack.

The original Slate artwork, palette and generated PNG/RGB565/mask pixel assets
are dedicated to the public domain under
[CC0 1.0 Universal](https://creativecommons.org/publicdomain/zero/1.0/).
The generator and C source code retain their separately stated code licenses.
Lucide icons have their own source/license records in `../icons/`; they are not
covered by the Slate artwork dedication.
The generated `study.png` also contains the existing Nano-X ROM font; its
license is preserved beside the preview and is not covered by this CC0 artwork
dedication. The atlas and individual skin PNGs contain no font glyphs.

Design at native logical resolution. Keep title text outside the bitmap, use
identical geometry for all states, retain a visible focus indicator, and inspect
the RGB565-quantized result. Source mask coverage is binary. Do not bake a
wallpaper into transparent corners: the renderer must preserve what is behind
them. Artwork masks alone do not implement native shaped windows or click-through.

Build the study with `python3 ports/fusede/tools/build_theme.py`. Generated assets
go to `build/fusede/slate/`, not this source directory. The 42×274 source atlas
currently takes 23,016 RGB565 octets plus 1,644 mask octets. Fonts/icons and
runtime backing surfaces are additional costs. Guest theme data is installed
under `/usr/share/fusede/themes/slate/` when `fdeshowcase` is included by the
normal application image builders. Its `.fdi` file is decoded by the guest client.
