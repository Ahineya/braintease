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
Texture initialization loads prepared definitions and column mappings into one
permanent zone allocation. Sprite image initialization loads final fixed-point dimensions and origin offsets
in one bulk read; final frame/rotation definitions are also prepared offline.
Pixel posts and texture composites still load or render when the renderer needs them.

Cooked format v5 includes all directory records, hash bucket heads and chains,
and the resolved `PNAMES` patch IDs. Preparation preserves case-insensitive
lookup, last-duplicate priority and missing unused patch names. The guest reads
bounded batches, checks ranges and indices, relocates pointers and copies the
finished records to permanent zone storage. It no longer decodes the directory's numeric
fields, hashes every lump name or searches for all 350 patch names at startup.
Native builds and the `packed`/`expanded` modes retain the original decoders.

The texture section adds all ordered patch references, dimensions, width masks,
fixed-point heights, column lump IDs and offsets, composite allocation sizes,
and texture-name hash chains. Unlike WAD lump names, duplicate texture names
keep the **first** definition. The normal guest checks the target ABI, section
and destination bounds, resolves pointer references and creates the animation
translation table. Prepared patch references copy directly to their final
storage; column mappings, width masks, heights and composite sizes load in one
contiguous span. It skips raw `TEXTURE1`/`TEXTURE2` decoding,
per-texture allocations and `R_GenerateLookup`. The shareware section contains
125 textures, 598 patch references and 10,776 columns in 36,638 disk words.

The sprite section stores the final frame counts, rotation flags, lump IDs and
flip flags in engine order. The guest reads one slab and resolves its pointers,
replacing the scan of every sprite name against every sprite lump in `P_Init`.
The shareware section contains 138 definitions and 261 frames in 4,867 words.
Changed sprite name lists and modified games retain the original resolver.
A separate 2,914-word section stores the final widths and signed origins for all
483 sprite lumps, including names unused by the game. It replaces per-sprite
patch-index lookups, header reads and fixed-point conversions with one allocation
and one contiguous payload read.

The renderer section stores projection, inverse projection, plane slopes,
distance scales and lighting indices for all 18 view-size/detail combinations.
Only the requested view's arrays are loaded. Light indices become pointers into
the guest's colormap allocation; requesting the current view again avoids disk
reads. The section occupies 106,004 words. The original-WAD fallback also uses
a single advancing inverse-projection scan and omits unused fixed-point multiplies.

Detailed validation happens **when producing the repacked image**. An independent
Node readback validator checks the serialized directory, hashes, PNAMES, every
cooked patch, texture, sprite definition and sprite metric against the embedded original WAD,
and renderer tables against the maintained source tables, before the packer
publishes the image. Its sidecar records the exact image, metadata
and validator identities. An unchanged build reuses that checked artifact;
changing the source, preparation modules, engine tables, fixed arithmetic,
validator or cached bytes triggers regeneration.
Staging retains the existing image identity check against the build manifest,
without repeating semantic validation. Neither `rbf` nor `rvm` knows anything
about Doom asset formats or validation.

`make prepare` produces the checked asset without starting a VM. The standalone
packer can also produce an image and validation sidecar, with any output name:

```sh
make -C ports/doom prepare
node ports/doom/tools/assets-pack.mjs ports/doom/assets/doom1.wad build/doom-cooked.wad cooked
```

This is Ripple's expanded/cooked disk format, not a replacement IWAD for other
Doom engines. `--wad` still selects the original IWAD; the generated file is
the VM's disk image. Use the artifact matching the guest's build manifest. The default
`ASSET_VALIDATION=preflight` means build-time validation. Diagnostic builds can
add the former per-texture, per-patch and per-column guest checks with
`ASSET_VALIDATION=guest`, or `--asset-validation guest` on either launcher.
Changing that policy invalidates existing objects and binary/BF exports.

Every Doom `calloc` now uses the existing fast clearing routine on Ripple,
including renderer buffers, patch-cache slots and tic data. Allocation sizes
are checked with full-width products without generic 32-bit division; renderer
allocation shares that check instead of calculating it twice. View and plane
arrays each share a typed permanent allocation, reducing those groups from 17
allocations to two. Required zeroing is preserved, including dirty reused memory;
arrays overwritten before their first read are left uninitialized.

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

With the bundled WAD, the v5 cooked image contains 968 validated patches and is
13,041,664 padded host bytes. A three-sample v1 BF benchmark copying 4,096 texture
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
of host speed; full BF startup had not been validated at that stage.
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
native reference. Those checks did not validate a full BF first frame.

The final bounded BF launcher check completed directory loading and was reading
the prepared hash table when its 300-second execution limit expired. It reached
1,771,279 guest instructions and 23,468 disk words, with the startup banner and
messages through `W_Init` printed. The last samples were around 2,500 guest
instructions per second. No frame had completed; this validates ongoing startup
progress, not a full BF boot or a first-frame ETA.

The v3 texture preparation closes a remaining gap in v2: picture patches were
prepared, but texture composition records and lookup tables were still built
by the guest. With the same WAD and title-start configuration:

| Stage | Cooked v2 instructions | Cooked v3 instructions |
| --- | ---: | ---: |
| Texture initialization | 15,739,194 | 4,833,593 |
| Complete `R_Init` | 19,805,769 | 8,859,875 |
| Startup through first title frame | 104,553,661 | 93,610,961 |

Zone allocations inside texture initialization fall from 674 to one. These
initial v3 measurements included detailed guest validation and pointer relocation;
pixel composites remain lazy.
The texture stage reads 41,694 words instead of 31,452, trading somewhat more
prepared data for 69.3% less guest instruction work. These are instruction
measurements, not BF wall-time or first-frame estimates.

The original-WAD oracle also passes an 80-texture, 80,000-column image spanning
several memory banks, including clipped overlaps, duplicate names and an empty
patch column. A separate two-texture/four-column fixture completed its cooked
initialization and reference checks in the BF host in 16.92 seconds under a
45-second limit, retaining 64,000-cell banks and 256 RAM banks. This validates
the new path on BF; it is not a full Doom boot benchmark.

Moving detailed validation to artifact production and bulk-loading the prepared
tables further reduces work, without changing the v3 disk bytes:

| Stage | Checked v3 guest instructions | Prepared-image loader instructions |
| --- | ---: | ---: |
| Texture initialization | 4,833,593 | 498,885 |
| Complete `R_Init` | 8,859,875 | 4,549,057 |
| Startup through first title frame | 93,610,961 | 89,299,153 |

Texture initialization performs 89.7% fewer instructions; complete `R_Init`
performs 48.7% fewer. Its patch-index read now happens when sprite initialization
first needs it, so total disk words through `R_Init` remain unchanged. The full
title-start reduction is 4.6%; these guest-work counts are not BF elapsed-time
predictions. In this build textures start near guest instruction 2,067,779 and
finish near 2,566,664; complete `R_Init` finishes near 6,527,425.

Both loaders match the original-WAD texture oracle on shareware and the
80,000-column fixture. All 37 texture mutations fail the native validator and
checked guest, and six structural mutations also fail the fast guest's retained
bounds checks. Title/menu/E1M1 captures and all 244 gameplay replay frames match
native output. The small BF fixture also passes under a 45-second execution
limit with the usual bank and memory sizes. No full Doom BF run was needed for
this change; allocation, pointer relocation, lazy pixel composites and later
game initialization still execute in the guest.

The v4 sprite and renderer preparation removes most of the remaining startup
work, with the same WAD, high detail and title-start settings:

| Stage | Prepared v3 instructions | Prepared v4 instructions |
| --- | ---: | ---: |
| Depth lighting tables | 2,366,377 | 364,356 |
| Complete `R_Init` | 4,549,057 | 2,522,146 |
| Sprite definitions | 27,029,359 | 66,540 |
| Complete `P_Init` | 27,267,504 | 304,685 |
| First view setup | 50,396,812 | 268,846 |
| Startup through first title frame | 89,299,153 | 10,183,279 |

This removes 88.6% of the guest work to the first title frame. Cumulative
milestones are approximately 1.98 million instructions entering `R_Init`,
4.50 million leaving it, 4.81 million leaving `P_Init`, 7.67 million entering
`D_DoomLoop`, and 10.18 million presenting the title. Disk reads rise from
214,977 to 228,414 words because the guest loads prepared results. These are
instruction counts, not a measured full-BF speedup or first-frame ETA.

Every renderer value matches frozen original native C for all 18 settings.
Forward and reverse view changes, unchanged-view reuse, and buffers whose first
wide value crosses a RAM bank are checked in the Rust VM. Sprite tables match
native C for shareware, an empty set and 4,002 synthetic frames spanning banks.
A one-frame sprite fixture also halts successfully in BF with identical native
and Rust VM output: 173,296 guest instructions in 20.20 seconds under a
90-second limit, using the usual 64,000-cell banks and 256 RAM banks.
A small prepared-view/lighting BF fixture also passes its native sampled
checksums and memory guards: 610,458 instructions in 67.67 seconds under a
120-second limit. Title/menu/E1M1 pixels and all 244 gameplay replay frames
match native C. These bounded fixtures do not establish a full Doom BF boot.

The v5 follow-up removes per-sprite header lookups, groups the permanent view
and plane allocations, and uses the compiler's direct wide comparisons:

| Stage | Prepared v4 instructions | Prepared v5 instructions |
| --- | ---: | ---: |
| Sprite dimensions/origins | 1,157,694 | 23,985 |
| Plane-buffer allocation | 319,436 | 272,461 |
| Complete `R_Init` | 2,522,146 | 1,322,182 |
| Complete `P_Init` | 304,685 | 294,866 |
| Startup through first title frame | 10,183,279 | 8,947,245 |

Sprite metrics require 97.9% fewer guest instructions; complete `R_Init` needs
47.6% fewer. Calls to the general heap allocator during `R_Init` fall from 26
to 10. The remaining plane cost includes clearing its 107,360 cells.
The compiler replaces late XOR/OR copies of proven zero operands and compares
wide values word by word. Through the first title frame, executed XOR/OR
instructions fall from 210,735 to 134,546, including the removal of all 41,546
XOR/OR-with-zero copies. This changes BF cost beyond the 12.1% overall reduction
in guest instructions; these counts do not establish a full-BF elapsed time.

Cumulative milestones for this build are 1,969,985 entering `R_Init`,
3,292,167 leaving it, 3,591,354 leaving `P_Init`, 6,441,897 entering
`D_DoomLoop`, and 8,947,245 presenting the title. The title requires 229,400
disk words overall. Sprite setup itself now reads 2,914 words in a contiguous
section instead of 6,988 words across an index and hundreds of small reads;
the patch index loads later when a picture first needs it.

A small compiler regression with unchanged 64,000-cell banks and 256 RAM banks
reduced BF interpreter dispatches from 19,237,091 to 295,608, despite only
reducing guest instructions from 731 to 700. All six alternating runs halted
with the same output under explicit timeouts. Median execution time was
0.156 seconds before and 0.00865 seconds after; this tiny fixture isolates the
cost of bitwise operations and is not a Doom-wide speed estimate.
Title/menu/E1M1 pixels and all 244 gameplay replay frames match native C.
Sprite metrics match original native decoding for shareware, signed-extreme
origins, an empty namespace, and 11,001 sprites spanning memory banks. The
six-sprite fixture also halts with identical native output in the BF host under
a 120-second limit (273,572 guest instructions). Full Doom BF is exported but
not executed for this pass.

To reproduce the bounded reader comparison:

```sh
node ports/doom/tools/benchmark-wad-read.mjs --timeout 60 --samples 3
```

`make -C ports/doom test-assets` checks serialized artifact validation, packing,
validation-policy changes and incremental storage-mode changes,
all 44 header words and 15 malformed metadata cases in the guest, as well as
the original-WAD texture oracle in both fast and checked guest builds,
37 malformed texture metadata cases rejected by the native validator and
checked guest loader, and the
independent asset oracle. It also compares prepared sprite definitions against
the original engine and all renderer tables against frozen original routines.
Sprite metric checks compare every value with native decoding, including signed
origins, empty namespaces and slabs crossing memory banks. The renderer-storage
fixture checks allocation counts, disjoint ranges, alignment and dirty zeroing.
The texture oracle checks every texture field, patch reference, column mapping
and hash lookup using the original WAD. For separate
80,000-column fixture spanning several banks, run:

```sh
DOOM_BUILD=build/doom-texture-banks node ports/doom/tools/texture-metadata-validate.mjs --synthetic
DOOM_BUILD=build/doom-sprite-banks node ports/doom/tools/sprite-metadata-validate.mjs --synthetic
DOOM_BUILD=build/doom-renderer-tables node ports/doom/tools/renderer-metadata-validate.mjs
DOOM_BUILD=build/doom-sprite-metrics node ports/doom/tools/sprite-metrics-validate.mjs --large
node ports/doom/tools/test.mjs fixtures --program renderer_storage
```

`make -C ports/doom test-kernels` also
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
scripts/run-bf-doom.sh --telemetry-lite
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
window or press Ctrl-C to stop. A user-confirmed run reached the first Doom title
frame after approximately 2,500 seconds (42 minutes); subsequent frames remained
slow. This is a manual milestone, not an automated full-Doom BF validation run.

`--telemetry-lite` prints a dot every five seconds and compact BF/guest counts
and rates every thirty seconds on stderr, plus a final summary when stopped.
BF counts optimized interpreter dispatches; guest counts observed Ripple
instructions. Use `--telemetry` for detailed PC, memory and disk progress.
The two modes cannot be combined; `--telemetry-interval` only applies to detailed
telemetry. Guest output remains on stdout. For a bounded capture attempt:

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
second. That run did not validate a full boot; these bounded measurements do not
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
| `tools/assets-validate.mjs` | Independently read back cooked assets before publication; compare serialized metadata and pictures with the original WAD |
| `tools/sprite-metadata.mjs`, `tools/sprite-metrics.mjs`, `tools/renderer-tables*.mjs` | Prepare final sprite definitions, dimensions/origins and immutable renderer tables; validate their serialized values before publication |
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
experimental launcher is separate from the regular build/test commands. A user
has confirmed the first Doom title frame; full BF gameplay and an automated
full-boot regression remain to be validated.
