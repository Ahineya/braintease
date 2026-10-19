# Microwindows / Nano-X provenance

* Repository: <https://github.com/ghaerr/microwindows>
* Pinned revision: `2bf6c0ad1f08af0f0eb6f391d0c1e1d7b7848cab`
* Upstream commit date: 2026-10-13T19:04:00Z
* Commit: `Merge pull request #202 from ghaerr/dlgtemplate`
* Archive: `vendor/microwindows-2bf6c0ad1f08af0f0eb6f391d0c1e1d7b7848cab.tar.gz`
* Source URL: <https://codeload.github.com/ghaerr/microwindows/tar.gz/2bf6c0ad1f08af0f0eb6f391d0c1e1d7b7848cab>
* SHA-256: `c751ae1db5d16dcdc333203a322b2e0b8e4c8f2bdcc869e0c19097e2ce4a2372`

The complete upstream archive is retained. `upstream/` contains an unmodified
subset: license/readmes, build configuration, headers, engine, Nano-X and NX11
sources, drivers, compiled fonts, documentation, Nano-X demos and nxroach image
assets plus the dialog resource header included by nxroach. `UPSTREAM.json` records every extracted file, archive prefix and hash.
Large optional font/dependency/data collections remain in the complete archive.
This is the pristine import and audit baseline. The C16 engine, server, client, NanoWM and launcher
adaptations are maintained separately in `adapt.py`, `server_adapt.py`,
`apps_adapt.py`, `roach_adapt.py`, `desktop_adapt.py`, `tetris_adapt.py` and `ripple/`; generated
sources live in the build directory. See [port status](README.md).

Verify offline:

```sh
python3 ports/nanox/tools/verify.py
python3 ports/nanox/tools/audit.py
```

The audit's diagnostics go under `build/nanox-audit/` and record the compiler
hash. Compilation failures are recorded findings, not a successful build.

The top-level upstream license offers MPL 1.1 or GPL v2 for Microwindows/Nano-X;
individual files and bundled assets retain their own notices and terms. Keep
those notices when adapting files. The full archive preserves attribution and
sources outside the selected subset.

The terminal atlas at `ports/graphics/fonts/rom8x16.inc` copies the 4096 bitmap
rows in `upstream/src/fonts/compiled/rom8x16.c`, changing only the C symbol and
formatting. Its source comment identifies the generated ROM font; no visual
edits are made. `tools/verify.py` compares every word with that pinned source.
The atlas retains the upstream license in adjacent `ports/graphics/fonts/LICENSE`.

NanoTetris retains the upstream game and rendering, with checked source
adaptations and `ripple/tetris-platform.*` for monotonic deadlines, controls,
signal wakeup and persistent scores. Adaptations correct out-of-range spawn
and collision coordinates, line shifting/clearing, and C16 score width. The
blocking hard-drop and line-clear delays are removed. Generated sources retain
the original copyright and license notices.
