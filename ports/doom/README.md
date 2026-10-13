# Doom on Ripple C

The DoomGeneric engine adapted to Ripple's 16-bit C bytes, banked pointers and
existing VM instruction set. This directory contains the engine, platform code,
build rules, shareware WAD and regression fixtures. No external Doom checkout,
patch application, Python installation or source-fetch step is required.

Current scope: single-player Doom at 320×200 RGB565, keyboard controls, 35 Hz
logical simulation ticks and silent audio. Development and tests use the Rust
VM. An experimental Brainfuck launcher uses the same port and guest ISA;
the regular port tests do not run it.

## Build and play

Requirements: the repository's Rust toolchain, GNU Make, Bash and Node.js 18+
(no npm dependencies for this port). A host C compiler such as Clang or GCC is
also required for native reference tests. macOS and Linux are the supported
build environments; set `CC=gcc` when using GCC. Building the Rust workspace may require the platform
libraries described by its crates.

From the repository root:

```sh
cargo build --release
cargo build --release --manifest-path src/ripple-asm/Cargo.toml
ports/doom/run.sh
```

The launcher builds the engine, stages a generated asset disk and starts the Rust VM.
It never opens the source WAD as a writable VM disk.

```sh
ports/doom/run.sh --scene e1m1              # start directly in the first level
ports/doom/run.sh --scene e1m1 --low-detail # half-width world rendering
ports/doom/run.sh --scene e1m1 --headless   # capture one frame and stop
ports/doom/run.sh --build-only
```

For direct builds:

```sh
make -C ports/doom -j4              # Ripple engine
make -C ports/doom -j4 native       # host reference engine
make -C ports/doom -j4 both START_MAP=1 LOW_DETAIL=1
make -C ports/doom clean
```

Outputs are under `build/ripple/engine/` and `build/native/engine/`. Set
`BUILD=/absolute/output/directory` to use a separate build tree, or
`WAD=/path/to/DOOM1.WAD` to build for another local IWAD. The launcher accepts
`--wad PATH`. Automated asset expectations use the bundled shareware WAD.
Set `DOOM_BUILD=/absolute/output/directory` when launching or invoking the Node
test tools directly. Build settings and tool/source changes invalidate the
affected outputs.

### Asset storage

The default `cooked` build derives a disk image from the original WAD. It keeps
one serialized octet per 16-bit disk word, then appends prepared directory,
name-lookup and graphics data in Ripple's in-memory layout. Reading those cells requires no byte
unpacking; cached pictures need no second allocation and conversion pass.
Texture initialization loads patch headers and column offsets, and sprite
initialization reads only dimensions and origin offsets. Pixel posts load when
the renderer needs them.

Cooked format v2 includes all directory records, hash bucket heads and chains,
and the resolved `PNAMES` patch IDs. Preparation preserves case-insensitive
lookup, last-duplicate priority and missing unused patch names. The guest reads
bounded batches, checks ranges and indices, relocates pointers and copies the
finished records to permanent zone storage. It no longer decodes the directory's numeric
fields, hashes every lump name or searches for all 350 patch names at startup.
Native builds and the `packed`/`expanded` modes retain the original decoders.

Every Doom `calloc` now uses the existing fast clearing routine on Ripple,
including renderer buffers, patch-cache slots and tic data. Allocation sizes
are checked with full-width products without generic 32-bit division; renderer
allocation shares that check instead of calculating it twice.

The directory also lives near the start of Doom's zone instead of beyond the
entire reserved 8-million-cell arena. It uses 22,752 cells plus one allocator
header and survives level changes. Shorter tape traversals benefit directory
loading and later resource lookups without reducing the available bank count.

```sh
ports/doom/run.sh --wad-mode cooked       # expanded bytes + prepared metadata/pictures
scripts/run-bf-doom.sh --wad-mode cooked --telemetry
make -C ports/doom WAD_MODE=expanded      # expanded bytes, runtime patch decoding
make -C ports/doom WAD_MODE=packed        # original packed-octet transport
```

All three modes use the same source WAD and unchanged storage MMIO. Logical WAD
offsets and lengths still count original octets. The generated image and its
manifest live under the build directory; launchers stage a verified writable
copy. Changing the source, storage mode or packer rebuilds the matching guest
configuration. Native reference builds always read the original WAD and decode
its patches, independently of the converted graphics path.

The cooked format is versioned and checked against the guest's source
fingerprint, lengths and layout. A per-lump index selects converted pictures;
other lumps retain the ordinary reader and decoder. Conversion validates patch
column and post bounds. This does not precompute maps, game state, texture
composites or rendering: those still run inside Doom. The larger disk image
trades host storage for less guest work without changing the ISA, CPU, bank
sizes or generic BF interpreter.

With the bundled WAD, the v2 cooked image contains 968 validated patches and is
12,779,520 padded host bytes. A three-sample v1 BF benchmark copying 4,096 texture
octets into RAM bank 136 measured:

| Storage mode | Read interval | Complete fixture |
| --- | ---: | ---: |
| Packed | 14.442 s | 18.587 s |
| Expanded | 0.341 s | 4.415 s |
| Cooked | 0.339 s | 4.488 s |

The read interval includes the fixed end-marker print overhead; the complete
fixture includes initialization and identical checksum verification. Every run
matched native/Rust VM output and guest instruction counts. Code remains in low
banks in this fixture, so its roughly 42× read improvement is not a full-Doom
speedup or first-frame ETA. Very small reads can lose to added setup overhead:
the short boundary-check fixture measured 1.141 s packed versus 1.572 s cooked.

In the initial cooked-format implementation, texture initialization fell from 57,297,463 to 17,306,277
guest instructions, and sprite metadata initialization from 69,514,839 to
1,172,927. Reaching the title frame fell from 236,489,409 to 118,787,106 guest
instructions. Title, menu-to-E1M1 and direct E1M1 pixels match the original-WAD
native reference. These instruction counts measure guest work independently
of host speed; full BF startup remains a separate validation milestone.
A bounded full-launcher BF run printed the startup messages through `W_Init`
and reached 1,315,145 guest instructions with 8,476 disk words read before its
180-second execution limit. It was still processing the WAD directory and had
no frame; the faster bulk-copy result does not remove the remaining directory,
renderer-table and drawing costs.

The v2 metadata and allocation changes further reduce guest work:

| Stage | Cooked v1 instructions | Cooked v2 instructions |
| --- | ---: | ---: |
| Directory load + hash construction | 3,046,300 | 991,057 |
| Before textures inside `R_Init` | 1,644,281 | 89,411 |
| Texture initialization | 17,306,277 | 15,739,194 |
| Plane-buffer allocation | 8,123,479 | 319,634 |
| Complete `R_Init` | 30,731,567 | 19,805,769 |
| Startup through first title frame | 118,787,106 | 104,553,661 |

The directory loader itself falls from 1,002,605 to 733,858 instructions.
A small guest assembly validator walks each batch once, avoiding repeated C
pointer normalization while retaining the original bounds and pointer checks.
Finished records are published in bulk to the permanent directory. Hash construction
falls by 87%, and the PNAMES loader takes 40,424 instructions for 350 prepared
IDs. Missing-index checks use bounds comparisons instead of XOR against
65535, and batch pointers advance linearly. Instruction counts alone do not
predict BF time: bitwise operands and memory access patterns also matter.
Title, menu-to-E1M1, direct E1M1 and the 180-frame gameplay replay match the
native reference. Full BF first-frame validation remains outstanding.

The final bounded BF launcher check completed directory loading and was reading
the prepared hash table when its 300-second execution limit expired. It reached
1,771,279 guest instructions and 23,468 disk words, with the startup banner and
messages through `W_Init` printed. The last samples were around 2,500 guest
instructions per second. No frame had completed; this validates ongoing startup
progress, not a full BF boot or a first-frame ETA.

To reproduce the bounded reader comparison:

```sh
node ports/doom/tools/benchmark-wad-read.mjs --timeout 60 --samples 3
```

`make -C ports/doom test-assets` checks packing and incremental mode changes,
all 32 header words and 15 malformed metadata cases in the guest, as well as
the independent asset oracle. `make -C ports/doom test-kernels` also
checks the word reader and directory validator against independent oracles,
including full-width values, bank edges, pointer tags and the calling convention.

The normal launcher uses paced time for a window and unthrottled logical time
for headless captures. `--clock stepped` accepts the VM's JSON control protocol
on standard input. `--input-script FILE.json` supplies deterministic keyboard
events. See [the device guide](../../docs/doom-platform.md).
Headless output includes a PNG, raw RGB565, build manifest and execution report
under `build/run/`. Use `--output DIR`, `--frame N`, `--timeout SECONDS` and
`--max-instructions N` to set capture paths and limits. Reaching a limit before
the requested frame is a failure. Run `ports/doom/run.sh --help` for all options.

### Brainfuck host

```sh
scripts/run-bf-doom.sh --telemetry
scripts/run-bf-doom.sh --build-only
scripts/run-bf-doom.sh --scene e1m1 --low-detail --telemetry
```

The repository launcher delegates to `ports/doom/run-bf.sh`. It checks release
tool freshness, builds the maintained engine, exports its existing objects as
instrumented Brainfuck, and stages a writable copy of the generated asset disk. The CPU and
guest instructions execute in BF; `rbf` supplies graphics, disk, logical clock
and keyboard devices. Both RAM and code retain the port's 64,000-cell bank size,
with 256 RAM banks. Logical ticks are unthrottled by default.

The window is enabled by default. There is **no default timeout**; close the
window or press Ctrl-C to stop. Startup and the first frame can take a long time,
and a successful full Doom boot in BF is not yet a validated milestone.
`--telemetry` reports ongoing execution on stderr. For a bounded capture attempt:

```sh
scripts/run-bf-doom.sh --headless --telemetry --timeout 60 \
  --screenshot ports/doom/build/run-bf/title.png
```

`--headless` only disables the window; add `--screenshot`, `--raw-frame` or another
capture option to stop at a completed frame. A timeout before that frame returns
a nonzero status and writes the execution report. `--timeout` bounds execution,
including BF tape initialization; compilation, export, source parsing and host
setup happen before that budget starts.

Add `--profile build/run-bf/profile.json` to write an optional breakdown of
generic BF operation timings and hot navigation-loop source offsets, including
when the run times out. Profiling adds overhead; use ordinary runs for speed
comparisons. See [profiling details](../../rbf/README.md#devices-and-automation).

The image is `build/ripple/engine/engine.bf` within this port. The linker writes
compact ordinary BF initializers directly, avoiding an enormous macro expansion
for millions of guest instructions. The guest ISA is unchanged; the CPU still
executes ordinary BF, including its direct-copy immediate-load implementation.
`DOOM_BUILD` selects another build directory; `DOOM_WAD` or `--wad FILE` selects
the source WAD. The disk copy and default `<scene>.json` report go under
`build/run-bf/`; use `--output DIRECTORY` and `--report FILE` to override them.
`--jobs N` controls compilation parallelism. Other host options, including
`--timeout`, `--capture-frame`, `--input-script` and `--clock`, pass through to
`rbf`. See `scripts/run-bf-doom.sh --help` and [the host guide](../../rbf/README.md).

For direct builds after building the release tools, use
`make -C ports/doom -j4 bf` or `make -C ports/doom -j4 TARGET=bf`.
The launcher reads the export's declared tape size and sets the host's tape
allocation limit accordingly. Doom needs hundreds of megabytes for the tape
alone; the expanded source and interpreter representation need additional memory.

The 2026-10-13 startup optimization pass reduced the BF image from 368 to
250 MiB without changing the bank size or RAM allocation. A bounded comparison
of the first 70,000 instructions against the Rust VM dropped from 85.5 to
5.3 seconds, including BF tape initialization but excluding parsing/setup.
Both runs checked every register at each instruction boundary, every ordinary
RAM store and final RAM contents. The compiler changes alter the instruction
mix, so this measures a startup prefix, not a whole-boot speedup.

The linker skips empty initializers and gathers startup code near the entry;
the compiler reuses global destination addresses; numeric table fillers keep
one base pointer per chunk. The BF CPU loads immediates by copying instead of
bitwise conversion, and the generic interpreter batches marker-loop cache
maintenance. None of these changes executes guest instructions natively.
Main initializes the numeric/state tables before printing the banner. That
initial pass's 60-second production run reached 150,518 instructions, beyond
entry to main at 124,766, but timed out inside table initialization with no
banner or frame.

A follow-up profile identified marker writes as the main cost in this phase:
nearby conditional branches rebuilt the entire code-tape prefix. Branches now
adjust only the suffix between their old and new positions, preserving raw
16-bit PC wrapping and PCB. A 100-iteration loop at instruction one million
fell from a median 3.20 to 0.42 seconds with identical output and 204 guest
instructions. This is a focused CPU measurement, not a Doom boot speedup.

The compiler now promotes immutable pointer locals and reuses resolved banks
within basic blocks. One 64-entry numeric filler shrank from 3,473 to 2,301
instructions, with loads reduced from 135 to 5. Generated state/object fillers
also retain a pointer per record; their combined code shrank by 34.4%. These
changes reduced the full BF image from 250 to 198 MiB without reducing banks
or memory. The follow-up production run sustained roughly 7,000–11,600 guest
instructions per second through table filling and printed the Doom banner,
`Z_Init`, `V_Init` and `M_LoadDefaults` within two minutes. It was stopped after
150.46 seconds and 970,842 guest instructions, with no completed frame.
Later initialization remained slower, around 2,000 guest instructions per
second. Full boot is still unvalidated; these bounded measurements do not
predict first-frame time. The BF interpreter remains generic, and all guest
instructions still execute through the ordinary BF CPU.

The final image also passed an 850,000-instruction comparison against the Rust
VM: every register at instruction boundaries, every ordinary RAM store and
final RAM contents matched. It reached `D_DoomMain` at instruction 841,552.
That diagnostic run took 73.13 seconds; stores accounted for about 41 seconds
and calls/returns for another 20, identifying the next optimization targets.
Post-banner PC samples landed in string/number formatting and allocator pointer
normalization. Diagnostic and production timings are separate measurements.

The WAD startup follow-up traced the long PC `30:6318..10507` region to the
port's directory reader. It was building an unused private index before Doom
read the same directory again. The file adapter now opens a bounded stream;
`W_AddFile` remains responsible for validating the header and every lump. It
reads 16 records per batch and uses the existing fast memory helpers. The
reader uses bounded 16-bit chunks, direct storage MMIO and single-check field
decoding. The standalone asset reader still supports `wad_open` with an index.

The existing name hash is now built before IWAD identification: six missing
optional names previously caused six complete directory scans. With the bundled
WAD, bounded Rust VM traces measured `W_AddFile` at 2,436,781 instructions,
down from 11,092,867 (78.0% fewer), and `DOOM Shareware` at instruction 5,711,683,
down from 16,991,548 (66.4% fewer overall). Directory disk reads halved from
20,236 to 10,118 packed words. These are guest-work measurements, not BF
wall-time or full-boot speedup claims. The ISA, CPU and bank sizes are unchanged.

`--telemetry` now includes `disk_read_words`, its per-second rate,
`disk_last_read`, `disk_pos` and `disk_idle~`. Positions are **disk block:word**,
not RAM bank:cell. Increasing reads show storage progress; a stationary read
counter with advancing guest instructions can indicate subsequent indexing or
other computation. The host caches 64 KiB disk blocks, so it does not perform
a file seek for every guest read. Diagnostics remain on stderr and are optional.
In packed mode one disk word carries two source octets; expanded mode carries
one. Cooked metadata and patch headers also contain full 16-bit words. Compare
elapsed time and guest work across modes, rather than treating their raw disk
word rates as equivalent source-byte throughput.

Validation covers odd offsets, 65,540-cell reads crossing disk and RAM banks,
decoder masks, bounds rejection, partial directory batches and duplicate-name
precedence. The small reader also passed in BF in 2.15 seconds with 52,433
guest instructions and 69 disk-word reads. Full native/Rust VM title, menu and
E1M1 captures matched. A separate 180-second full BF run printed the startup
banner through `M_LoadDefaults` and reached 1,129,699 instructions, timing out
before directory loading; it does not validate a completed BF WAD load or frame.

A subsequent trace separated two apparent I/O stalls. Before WAD loading,
formatting 24 numbered configuration names consumed almost the entire binding
phase. Constructing those suffixes directly preserves all bindings and defaults
while reducing `D_BindVariables` from 314,289 to 5,032 guest instructions.
The general formatter also batches literal, string and padding runs; a small
literal-format BF fixture fell from 0.98 to 0.63 seconds with identical output.

After six disk words (the WAD header), the sampled `30:14260..14266` loop was
clearing 22,752 cells in RAM bank 136. The directory decoder now initializes
each field without clearing the allocation first, eliminating 20,228 store
instructions overall. This matters more than the modest change in guest count:
each far store builds and clears roughly 8.7 million BF marker cells. A local
256-store benchmark took 3.20 seconds; larger interpreter batches offered little
benefit or regressed. Both loops are already recognized by generic optimizers.
The interpreter, CPU, ISA and bank sizes are unchanged in this pass. Tests cover
nonzero reused allocations, configuration names/targets/defaults, formatter
truncation and wide counts, and matching native/Rust VM title, menu and E1M1.

The rebuilt image's 180-second execution test printed `W_Init` at approximately
73 seconds on the telemetry clock and stopped at 1,047,257 guest instructions
with 646 packed disk words read. Directory batches were advancing beyond the
header, but WAD loading and the first frame had not completed. The additional
five- to tenfold host-rate drops reported in longer runs were not reproduced
locally; these changes do not establish their cause.

The directory-decoding follow-up keeps decoded positions and sizes local while
checking them, then writes each finished entry to the far heap. It avoids
re-reading distant fields and repeated checked-helper calls for an already
bounded record. Shared little-endian decoding assembles native 16-bit halves
before widening, preserving the masks on serialized octets.

With the bundled WAD, `W_AddFile` fell from 2,426,603 to 1,756,192 guest
instructions (27.6% fewer), and far-heap reads fell from 7,965 to 381 (95.2%
fewer). The 22,790 heap writes and 10,118 disk-word reads are unchanged.
`DOOM Shareware` now completes at guest instruction 4,721,645, down from
5,392,056. These counts come from the rebuilt production image, not an
isolated decoder. Tests cover full-width fields, ignored upper cell bits,
overflowing ranges and poisoned reused allocations; native/Rust VM title,
menu and E1M1 pixels still match.

The generic BF engine also defers proven uniform tape runs and cancels writes
when cleanup restores their backing values. This targets repeated marker work
in distant memory accesses without recognizing guest instructions. See the
[engine design](../../rbf/README.md#execution-engine) and
[fixed-bank benchmark](../../rbf/benchmarks/README.md#deferred-uniform-tape-runs-2026-10-13).

### Controls

| Action | Keys |
| --- | --- |
| Menus | Escape, Enter, arrows |
| Forward / backward | Up / Down or W / S |
| Turn | Left / Right |
| Strafe | A / D, comma / period, or Alt + Left / Right |
| Fire | Control (⌃ on macOS, not Command) |
| Use door / switch | Space |
| Run | Shift |
| Automap | Tab |
| Select weapon | Number keys |
| Toggle high / low detail | F5 |
| Shrink / enlarge view | Minus / equal |

Both left and right modifiers work. Close the window to quit. High detail is
the default. Low detail draws adjacent world pixels in pairs; the display,
menus and status bar remain 320×200.

## Where the code comes from

The sole engine base is [ozkl/doomgeneric](https://github.com/ozkl/doomgeneric),
revision `e257a09527062bfb986b6e5e0780839b6cbd5b71`. Doom8088 contributes no
code and is no longer a dependency or reference checkout. See
[`upstream.json`](upstream.json) for the revision and original file hashes,
[`LICENSE`](LICENSE) for the upstream license, and each source's copyright
notice. The WAD is separate game data: see [`assets/README.md`](assets/README.md).

We compile **69 of the 81 C files** selected by DoomGeneric's default Makefile,
plus our adapter and generated initializers. This retains the real game loop,
menus, automap, player movement, monsters, combat, collision, BSP world renderer,
textures, sprites, lighting, status bar, intermissions, resource manager and zone
allocator. It is the Doom engine, not a replacement renderer or game simulation.

At consolidation, 27 of those 69 files are byte-for-byte upstream; 42 contain
port changes. A line comparison retains 48,434 of 49,787 original lines (97.3%).
This is a source-line measure including comments and inactive table definitions,
not a claim that 97.3% of every upstream feature is enabled.

The 12 omitted source files are `doomgeneric.c`, `doomgeneric_xlib.c`,
`i_input.c`, `i_scale.c`, `i_system.c`, `i_timer.c`, `i_video.c`, `m_config.c`,
`w_file_stdc.c`, `f_wipe.c`, `sha1.c` and `w_checksum.c`. Host-specific services
are supplied below; wipes and WAD checksumming are omitted in this profile.
The convenience `doomgeneric_Create` / `DG_*` wrapper is not compiled.
Our entry point calls `D_DoomMain` and the retained `doomgeneric_Tick` function
in `d_main.c`, while implementing the engine's lower-level `I_*` platform API.

The former 24 patch files have been folded into `engine/`. Edit the engine
sources directly; do not recreate an ignored patched source tree.

## Port architecture

```text
assets/doom1.wad       engine/                       src/main.c
      |               Doom gameplay + renderer <---- initializes tables,
      |                       |                      enters D_DoomMain
      v                       v
wad_io.c +            platform.c + config_port.c + patch_cache.c
w_file_port.c         clock, keyboard, indexed frame -> RGB565
      |                       |
      +-------- runtime C device/storage API --------+
                              |
                        unchanged Ripple ISA
                              |
                       Rust VM host devices
                       window / capture / disk
```

| Code | Responsibility |
| --- | --- |
| `engine/` | Adapted DoomGeneric C sources and headers; explicit arithmetic widths, safe serialized decoding, heap-backed large arrays and renderer cache improvements |
| `src/main.c` | Initialize canonical tables and state/object definitions, select startup mode, enter the real engine |
| `src/platform.c` | System hooks, Doom zone allocation, logical time, keyboard translation, palette and RGB565 output; deterministic native capture path |
| `src/config_port.c` | Explicit control bindings and defaults without persistent host configuration |
| `src/wad_io.c`, `src/w_file_port.c` | Checked octet reads from the configured IWAD and a Doom file-backend adapter |
| `src/doom_decode.h`, `src/patch_cache.c` | Explicit WAD structure decoding and cached decoded pictures |
| `src/asset_metadata.c`, `src/alloc_fast.c` | Validate and relocate prepared directory/hash/PNAMES data; checked zeroed allocations using fast memory routines |
| `src/compat_libc.c`, `include/` | Missing guest C library functions and target-compatible headers; unsupported persistent file operations fail explicitly |
| `src/*_fast.asm` | Guest assembly for fixed-point arithmetic, raster loops, patch/palette operations, memory copies, expanded storage reads and cooked metadata validation; ordinary Ripple instructions |
| `tools/generate-data.mjs` | Turn canonical trig tables, 967 states and 137 object definitions into bounded heap-initializer functions |
| `tools/assets-pack.mjs` | Validate the source WAD and generate expanded/cooked disk images and their manifests |
| `tests/`, `tools/` | Native/VM comparisons, independent asset and kernel oracles, captures and profiling |
| `tests/titlepic/` | Earlier C/BFM title-screen fixtures; neither is the full game |

The build uses Make for compilation, assembly and linking, Bash for launching,
and dependency-free Node helpers for deterministic data generation, WAD staging
and test orchestration. Generated code, logs, objects, binaries and screenshots
stay under ignored `build/`.

### The 16-bit memory model

A Ripple C byte is a 16-bit cell. `sizeof`, allocation and memory functions count
cells. A serialized WAD byte is an **8-bit octet**, represented as a cell with a
value in 0..255. File record lengths and offsets count octets. The port explicitly
decodes original WAD fields. Versioned cooked records are generated
for the target ABI, checked before use, and have their pointers relocated;
native reference builds continue to decode the original WAD.
The legacy `uint8_t` alias in the private headers is not an exact 8-bit C type.

Doom's `fixed_t`, angles and wide arithmetic use software 32-bit operations.
Pointers contain a bank and an offset. Large renderer arrays and the
8,388,608-cell Doom zone are on the banked heap. The indexed screen and RGB565
output each occupy 64,000 cells. The VM is provisioned with 256 banks of 64,000
cells, or 16,384,000 cells total. There is no 32-bit VM extender.

Each function and the combined static-data area must fit their respective bank
limits. Canonical data therefore becomes 59 generated C units containing small
initializer functions, rather than oversized static arrays or one enormous
startup function. Values and state action callbacks are preserved.

The assembly helpers assume this port's 64,000-cell banks, 320-cell screen rows,
normalized pointers and decoded texture samples. They use the existing ISA and
C calling convention. Native builds use corresponding C implementations, which
are checked against the guest kernels. The compiler's general pointer paths
remain independent of these port-specific assumptions.

### Time, input and rendering

The loop requests one logical tick grant, processes the input batch and advances
Doom. Clock reads do not advance time. The host can pace at 35 Hz, grant ticks as
fast as execution allows, or wait for automation to grant them. A slow backend
therefore progresses through the same game ticks without having to catch up
with an ever-growing wall-clock backlog.

Input arrives as USB HID transitions and held-key state, translated into real
Doom events. Doom renders its indexed-color image; the adapter converts through
the current palette and presents RGB565 via `RGB565_EXT`. Capture sees the same
completed frame as the display window. The Rust VM has no Doom-specific opcodes
or execution shortcuts.

### What is librvm?

`target/release/librvm.rlib` is Cargo's **host-side Rust library** for the `rvm`
crate. It exposes the interpreter, runner, devices and capture APIs. The `rvm`
executable and the port's Rust test/profiling drivers use it.

It is not guest code and is not linked into Doom. The guest instead links
`runtime/crt0.pobj` and `runtime/libruntime.par`: the startup code and C runtime
compiled into Ripple instructions. `librvm.rlib` runs those instructions on the
host. Both libraries are generated build products.

## Automatic checks

```sh
make -C ports/doom test
make -C ports/doom test-fixtures
make -C ports/doom test-kernels
make -C ports/doom test-gameplay
```

The default `make test` compares title, menu-to-E1M1 and direct E1M1 output with
the native reference. Foundations cover numeric tables, libc, WAD/zone/cache
operations, malformed data, controls and an independent WAD/image oracle.
Portability covers isolated map/world arithmetic and 66,000 logical tick grants.
Kernel checks use independent Rust oracles for arithmetic, pixels, memory,
bank crossings, guards and the calling convention. Gameplay compares 180 frames
of movement/combat/automap plus arrow-key and F5-toggle replays with native C.
This caught a compiler bug that truncated wide `switch` selectors and prevented
the automap's status-bar refresh. The selector now retains its promoted width;
`./rct switch_wide_control` covers signed 32-bit and unsigned 32/64-bit dispatch.
For direct invocation, use `node ports/doom/tools/test.mjs fixtures` (or
`smoke`, `kernels`, `gameplay`). Add `--output DIR` to retain a separate run;
`--help` lists limits and options.

Test manifests identify the source WAD, storage mode, padded disk image,
executable and replay. The staging helper verifies the generated asset image
and copies it into the test output directory. Disk images are padded to
65,536-octet storage blocks while preserving the logical WAD length.
Mismatches retain captures and logs. The native reference
shares the adapted engine, so independent asset and kernel checks remain useful
in addition to pixel equality.

The small platform suite remains `scripts/test-visual-smoke.sh`; pass a WAD to
include the older TITLEPIC fixture. Existing `scripts/run-doom-c.sh` and
`scripts/run-bfm-doom.sh` still launch those fixtures. Use `ports/doom/run.sh`
for the complete game.

## Performance measurements

```sh
make -C ports/doom -j4 both START_MAP=1
node ports/doom/tools/benchmark.mjs --output ports/doom/build/perf-high \
  --profile-frame 20 --archive runtime/libruntime.par
```

The benchmark uses already-built engines with matching scene/detail settings,
compares every frame, records production-runner wall times and guest instruction
counts, and profiles separately when requested. Add `LOW_DETAIL=1` to the build
for low-detail comparisons. Add `--input ports/doom/tests/gameplay-long.json
--frames 180` to measure the longer replay. Avoid concurrent builds when
comparing wall time; instruction counts are deterministic.

For historical context, a paired high-detail 32-frame E1M1 run measured 1.40×
from runner batching with identical guest instructions, and 1.55× including
compiler spill cleanup. Median instructions fell from 9,354,466 to 8,482,426
per steady frame. These headless measurements are not a windowed FPS guarantee.
Local evidence remains in ignored `build/batching-perf/`; compiler details are
in [the optimization notes](../../rcc-backend/OPTIMIZATIONS.md).

## Remaining scope

Save/load, demo recording, multiplayer, sound, mouse input, persistent host
configuration, PWADs and melt wipes are deferred. Longer sessions, additional
maps and demo playback need broader validation beyond the bounded E1M1 replays.
The Brainfuck host supports the device interface and multi-bank export. Its
experimental launcher is separate from the regular build/test commands; full
Doom boot and gameplay on that backend remain to be validated.
