# QR codes on the 80×30 terminal

The portable C engine and `lib/qr` RFL binding encode at most **84 octets** as
byte-mode QR, medium error correction, versions 1 through 5. They allocate a
bounded matrix, use no I/O/host service and copy the input. A 60-octet payload
uses version 4 (33×33 modules); with the mandatory four-module white border,
its display takes **41 columns × 21 rows**. The largest supported matrix takes
**45 columns × 23 rows**. Oversized inputs fail before encoding.

The encoder is Project Nayuki's MIT-licensed C implementation, release v1.8.0,
with the two C16 portability changes listed in [vendor/README.md](vendor/README.md).
Heavy loops stay in C. RFL requests one packed row at a time and sends each
row to `Screen.cells` in one call.

`qr_new(bytes, length)` returns an owned opaque matrix, or NULL. `qr_columns`
and `qr_rows` include its white border. `qr_row` fills a caller-supplied row
buffer atomically and retains only the curses color-pair bits of its style;
use black foreground and white background. `qr_free` releases the matrix.
Characters are space plus Unicode full/upper/lower block cells
(`U+2588`, `U+2580`, `U+2584`).
Core curses emits these as UTF-8 `█`, `▀`, `▄`, one terminal cell each. The
graphical VT recognizes those sequences across fragmented reads and uses its
shared Unicode bitmap font. External terminals need a UTF-8 font with contiguous
half blocks. See [Unicode support](../unicode/README.md) for the wider text API.

Use the entire rectangle. Clipping a QR, altering its aspect ratio, applying
bold/underline, or drawing into its quiet zone can make it unreadable. On the
built-in 8×16 font, each module is an 8×8 square. The demo refuses a screen that
cannot fit the whole rectangle and redraws after resize.

```rfl
import { encode } from "lib/qr";
import { BLACK, WHITE, color_pair } from "std/curses";

// With an active screen and enough room for the entire rectangle:
screen.colors();
screen.define_pair(1, BLACK, WHITE);
let qr = encode(payload)?;
for (row in 0..qr.rows()) {
    screen.cells(top + row, left, qr.row(row, color_pair(1)));
}
```

## Try it

```sh
make -C fuse-x "$PWD/fuse-x/build/apps/qr-demo.fxe"
./scripts/run-fuse-x.sh
```

At the guest shell, run `qr-demo` and scan it with a phone camera. Its default
payload is a harmless example.org test URL. `qr-demo 'short text'` displays
other payloads; `q` exits and `r` redraws. Standard Fuse-X image installation
includes the demo when built. The demo does **not** contact Telegram or create
a login token. Future QR authorization must refresh tokens at server-supplied
expiry, handle DC migration and 2FA, and preserve session state.

## Validation

```sh
python3 libraries/tests/test_qr.py --target host
python3 libraries/tests/test_qr.py --target both
python3 ports/graphics/tests/run.py
```

The host checks cover every payload length 0..84, binary octets, boundary
guards, atomic invalid writes and the white border. Disposable Fuse-X/RVM
guests compare their matrices with the host build, render in 80×30, reject an
undersized screen, recover after resizing, and restore terminal modes on exit.
The captured ANSI stream is replayed byte by byte through the real portable
VT and rasterized with nxterm's ROM font. macOS additionally scans that image
with Apple's independent Core Image QR decoder and checks the exact payload.
Images and terminal captures are written to `build/qr-test/`.
