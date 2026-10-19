# FuseDE icons

FuseDE uses a small, pinned selection of [Lucide](https://lucide.dev/) icons as
**tintable raster masks**. The guest needs neither an SVG engine nor an icon-font
renderer. Names such as `folder`, `terminal`, `settings` and `save` identify the
artwork; no private Unicode codepoint is part of the application interface.

The 20 unmodified SVGs in `lucide/` come from release **1.59.0**, revision
`1ef60af944de207e0d5176905a0759ba12e6ee8d`. `lucide/UPSTREAM.json` records the
upstream paths and SHA-256 of every imported file. `lucide/LICENSE` retains the
ISC license and the separate Feather-derived MIT notice. These icons are not
part of Slate's original CC0 artwork dedication.

## Ordinary offline builds

```sh
python3 ports/fusede/tools/build_icons.py --output build/fusede/slate
```

The builder verifies the pinned source hashes and uses the checked-in
`masks.json`, producing `icons.h`, `icons.mask`, `icons.fdi`, `icons.json`,
`icons.png` and `icons-LICENSE`. It needs only Python's standard library.
The atlas includes all 20 icons at **16, 24 and 32 pixels** and occupies
6,912 physical octets of mask data. A one-pixel gutter surrounds every icon.
Mask coverage is binary, with 16 pixels per MSB-first word and padded rows.
The image service applies the caller's foreground colour, so selected/disabled
icons share the same masks. Icons can be stretched, but these authored sizes
should normally be used directly for crisp outlines.

The generated header supplies symbolic indices and coordinates. It is source
metadata for applications to rebuild against, not a stable numeric IPC enum.
The JSON supplies equivalent named rectangles for future dynamic loaders.
Installed files live in `/usr/share/fusede/icons/lucide/`, including `LICENSE`.
The showcase loads `icons.fdi` from that directory by default.

## Updating the raster source

Maintainer-only regeneration uses [sharp](https://sharp.pixelplumbing.com/)
(currently 0.35.5, libvips 8.18.7 and librsvg 2.63.2). The raster manifest records
the versions, 4× supersampling and alpha threshold 128. This dependency is used
on the host to author the baked data; it is not a build or guest dependency.

```sh
FDE_SHARP_MODULE=/path/to/sharp bun ports/fusede/tools/rasterize_icons.mjs
python3 ports/fusede/tools/build_icons.py
```

To change the selection, first import the unmodified SVGs and license from an
explicit upstream commit and update `UPSTREAM.json`. Regenerate `masks.json`,
inspect all sizes, and run the asset and guest tests. Keep old source/license
notices when required. Do not silently replace a pinned SVG during an ordinary
build, fetch from the network at boot, or install an entire font collection.

## Icon fonts later

[Nerd Fonts](https://www.nerdfonts.com/) aggregates many icon/glyph sources and
patches fonts; its symbols-only distribution is another possible source. The
first desktop has an existing bitmap text font and no general Unicode/icon-font
pipeline. Adopting a font requires codepoint mapping, glyph metrics/rasterization,
fallback and per-source license tracking. That work is separate from showing a
small named GUI icon. A later host font exporter can produce the same mask format,
and a future font renderer can implement the same named-icon interface.

Lucide outlines suit toolbars, menus, file lists and window controls. Large
illustrated application/file-type icons can be added as RGB565-plus-mask assets
through the same image service; a separate art source need not change clients'
window or drawing model.
