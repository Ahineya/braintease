# RVM in the browser

The original Ripple interpreter, MMU, interrupt controller and bulk device
validation compile to WebAssembly. A dedicated worker owns the machine, RAM
disk and framebuffer. The page boots to an 80×30 FUZIX console. Run `startx`
to open Nano-X, NanoWM and NXDSKTOP; leaving the desktop returns to the console.
FuseDE is not included.

## Build and run

Requirements: Rust with `wasm32-unknown-unknown`, Node/npm, Python 3, and
the repository's RCC toolchain and compiled FUZIX application artifacts under
`build/fuzix`. The build compiles its own kernel and BIOS under
`build/rvm-web/fuzix`; it does not boot, read or update a normal guest image.

```sh
rustup target add wasm32-unknown-unknown
python3 rvm/web/build.py
python3 rvm/web/serve.py
# Open http://127.0.0.1:8086/
```

The browser build uses its own Cargo cache in `build/rvm-web/cargo`, so it can
run alongside native/RFL builds. xterm.js is pinned by `package-lock.json` and
bundled locally; the page does not load scripts from a CDN. The HTTP server
serves only `build/rvm-web/site`, never the repository root.

`image.mjs` creates a new disk from its explicit executable allowlist and
source-defined configuration. It has no input-image option. It includes no
Telegram application, credentials, host-folder mount, or FuseDE shell. The
seed is checked before compression, and `seed-manifest.json` records its
executable sources. Boot and payload hashes are in `build.json`.

## ChatGPT Site

The public deployment is
<https://ripple-rvm-browser.p-yevsehnieiev.chatgpt.site>.
Its existing Site identity is retained in `.openai/hosting.json`; reuse that
project for updates rather than registering another Site.

Publishing uses a separate Git checkout in `build/rvm-web/hosting`, containing
only its hosting manifest and the explicit browser assets under `dist/`.
The main repository, normal guest disks, test exports, build caches, and source
provenance files are not part of that checkout. Rebuild the browser bundle
before updating its assets, then use the Sites workflow to push, package, and
deploy the same source commit. Public access was enabled at the owner's
request on 2026-10-18; visitors do not need to sign in. Preserve that audience
for future updates unless a different sharing policy is requested.

Hosted verification on 2026-10-18 confirmed console boot, file I/O, manual
desktop startup, graphical keyboard and mouse input, desktop exit, clean
shutdown, browser disk download, and reimport with the saved file intact.
No browser console warnings or errors were reported during those checks.

## Using the machine

- Type in the console normally, including control keys and clipboard paste.
- Run `startx` to open the desktop. Click its canvas to focus keyboard input;
  pointer coordinates follow CSS scaling. NanoWM owns guest window movement
  and resizing. `startx --minimal` uses the smaller launcher.
- Run `rift /notes` or `vi /notes` for Rift, `mc` for RFL Commander, and
  `mux` for terminal tabs and split panes. Mux help is Ctrl-B followed by `?`.
- Unix tools include `awk`, `sed`, `grep`, `find`, `diff`, `tar`, `make`, and
  pagers. BASIC, Tcl, Brainfuck, Sokoban, Wump, and Hamurabi are included too.
  Use `man COMMAND` for the installed manuals and `cat /welcome.txt` for a tour.
  Editor/file-manager themes and BASIC examples come from repository resources.
- Pause suspends execution; Resume continues the same machine.
- Files live only in this tab's RAM disk. Run `shutdown`, then **Export disk**
  to download a clean FUZIX image containing your changes. Reset disk discards
  this tab's changes and boots the original seed. Closing/reloading loses
  unexported changes.
- **Import disk** opens an exported Ripple/FUZIX `.img` file (up to 256 MiB).
  Format, FAT chains, kernel layout, and C16 V3 geometry are checked before
  asking to replace the current session. Cancelling or choosing an invalid
  file leaves the current machine running. The original file is never modified
  or uploaded; changes live in the tab until exported again. Reset always
  returns to the bundled seed. Automatic persistence is future work.

The initial profile is offline. Ethernet, audio, the crypto coprocessor and
host folders are unavailable; unsupported devices fail explicitly. Entropy
uses browser `crypto.getRandomValues`. The Wasm module imports only clocks and
entropy. No SharedArrayBuffer, cross-origin isolation, host helper or server
execution is required.

The public application list is maintained in `apps.mjs`. Network clients,
audio players, FuseDE tools, and development self-tests are excluded. Executable
aliases share files instead of duplicating them. The disk is 256 MiB with a
224 MiB C16 root; the extra space accommodates the offline applications and
their resources without changing the browser disk limit. The native compiler
SDK is not part of this image.

## Architecture

`rvm/src/web` provides the worker ABI and browser host adapters. ISA execution,
protected memory, instruction uploads, syscalls and graphics use the same
reference implementation as native RVM. Native Cranelift and desktop packages
are target-specific dependencies. RGB565 state is shared with the native
renderer; only its window/event-loop implementation lives in
`display_rgb565_window.rs`.

Execution turns are bounded by both instructions and elapsed time. Clock
samples are refreshed at bounded instruction intervals, avoiding JavaScript
calls per guest instruction. HOST_IDLE returns a scheduling hint rather than
blocking the worker. Console bytes and input events cross in batches. Completed
RGB565 frames convert to RGBA inside Wasm and transfer to the page as one buffer.
No synchronous host import may reenter the VM.

Immediate worker turns use a message queue, avoiding nested browser timer
delays. The kernel applies backpressure when its graphical input ring fills,
so a slow repaint does not discard ordinary keyboard transitions.

## Verification

```sh
node rvm/web/tests/smoke.mjs
node --test rvm/web/tests/disk.mjs
node rvm/web/tests/import.mjs
cargo check -p rvm --lib
cargo test -p rvm --lib display_rgb565::
python3 ports/fuzix/tools/fsck.py build/rvm-web/tests/export.img
```

The smoke test runs the actual Wasm module and fresh guest image in Node. It
checks console-first boot, guest file round trips, a Unix pipeline, mux sessions,
Rift editing and saving, Commander startup, malformed input rejection, manual
desktop startup, typing in a graphical terminal, return to the console, and
shutdown. Browser interaction also needs checking in a real browser; Node
cannot exercise browser DOM input, canvas delivery or the file chooser.
The import test uses the actual worker in Node, reopens an exported disk,
writes and exports another file, reimports it, and checks reset to the seed.
