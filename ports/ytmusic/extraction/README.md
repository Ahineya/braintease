# Restricted QuickJS extraction experiment

This is an offline compatibility and performance probe, not live YouTube URL
extraction or playback. It uses pinned, pristine upstream EJS and YouTube.js
modules and a public player fixture. Five expected outputs come from EJS's own
tests: three `n` transformations and two signatures, including control characters.
Both pristine host QuickJS and the actual C16 Fuse-X guest must match them.
A deliberately wrong expected output must fail with exit status 1. The guest
allocator must have no live allocations after runtime teardown.

## Run

Requirements: existing RCC and Fuse-X build prerequisites, pristine QuickJS
sources in `ports/quickjs/upstream`, Node/npm and a host C compiler. Network
access is used only to prepare pinned public sources/dependencies. The guest
does not require Node, npm, an account, or a remote extraction service.

```sh
python3 ports/ytmusic/extraction/prepare.py
python3 ports/ytmusic/extraction/check.py --backend youtubejs --memory-mode read-v1
```

On macOS, set `SDKROOT` to the installed macOS SDK if the host compiler requires
it. `--backend ejs` selects the upstream EJS preprocessing route.
`--host-only` runs only pristine host QuickJS. `--cached` is a **diagnostic**:
it preprocesses the player with Node on the development host, then tests the
prepared script in guest QuickJS. It does not prove cold guest extraction and
is not a proposed production dependency. A production cache must be prepared
by the guest itself and invalidated by player content and extractor version.

Sources, bundled scripts, logs and reports stay in ignored
`build/ytmusic-extraction`. Guest runs use disposable disk images and private,
matching libc libraries; installed user disks are untouched. Avoid concurrent
runs using the same build directory; a build-directory lock rejects them.
`--skip-build` requires matching previously
built bundles, reference binaries and probe SDKs.

### Performance regression suite

```sh
python3 ports/ytmusic/extraction/regression.py --label current
python3 ports/ytmusic/extraction/regression.py --label cold --full
```

The default runs both prepared-script catastrophes with the real guest, exact
upstream outputs, negative controls, allocator teardown and filesystem checks.
`--full` additionally runs cold preprocessing for both backends, with a
600-second JS deadline per invocation; a cold timeout fails the suite rather
than becoming an expected success. `--backend youtubejs` selects just that
backend. Cold preprocessing may take many minutes, including its negative
control. No live YouTube request or login is involved.

Reports record compiler, generated engine, executable, libc, kernel and bundle
hashes, phase timings and peak engine allocation. Labels retain separate small
reports while reusing two private probe SDKs. `performance-baseline.json` keeps
the original pre-optimization prepared measurements. `--enforce-baseline`
rejects timing/heap regressions beyond `--max-slowdown` (default 1.25); use this
only on the same machine, since these historical single runs are not portable
benchmarks. Correctness checks and invocation deadlines always apply.

For runtime changes, build a private test kernel and set `FUSE_X_TEST_KERNEL`
to its absolute path. This avoids replacing the user's installed kernel:

```sh
cargo build --release -p rcc-driver -p rcc-native-runtime
make -C fuse-x BUILD="$PWD/build/ytmusic-extraction/kernel" \
  "$PWD/build/ytmusic-extraction/kernel/fuse-x"
FUSE_X_TEST_KERNEL="$PWD/build/ytmusic-extraction/kernel/fuse-x" \
  python3 ports/ytmusic/extraction/regression.py --label current
```

## Results, 2026-10-22

Fuse-X measurements below used explicit `read-v1` memory lowering. These are
single experimental runs, not a benchmark suite. Both backends use the same
player ID and five expected vectors, but different upstream player variants:
EJS uses `player_ias`, YouTube.js uses `player_es6`. Report hashes identify them.

| Prepared-player execution | EJS | YouTube.js dependency slice |
| --- | ---: | ---: |
| Prepared source size | 3,666,615 characters | 179,957 characters |
| Guest compile/execute time, coarse guest clock | 103 s | 3 s |
| Guest process wall time, including loading/setup | 119.5 s | 7.2 s |
| Peak tracked QuickJS allocation | 43,752,563 cells (83.5 MiB) | 2,997,334 cells (5.7 MiB) |
| Correct vectors / failing negative control | 5 / passed | 5 / passed |

Peak allocation includes engine allocations tracked by the C16 QuickJS
allocator, not total process memory, input buffers or host runtime overhead.
Each C16 cell occupies two physical bytes. These numbers include compilation
of the prepared code; they do not measure isolated signature-call latency.

Cold EJS preprocessing did **not** pass: the checked run hit a 180-second
deadline, and the `read-v1` run hit 600 seconds while parsing the full player.
Neither result establishes a practical cold-start path. Both returned to the
shell and the disposable filesystems passed checks. Prepared-script success
must not be presented as a self-contained extraction pass.

YouTube.js emits a much smaller dependency slice, but its cold preparation
still parses the full source with Meriyah. Its full `read-v1` guest run also
hit the 600-second parsing deadline, with 35,433,555 cells (67.6 MiB) of peak
tracked allocation at interruption. It released all tracked allocations,
returned to the shell, and the disposable filesystem passed checks. This is
an incomplete parse, not its eventual peak or a successful cold extraction.
Cold guest preprocessing, additional player versions and live guest HTTPS
extraction remain acceptance gates.

### Controlled optimization comparison

Sequential, single-guest runs on an otherwise idle Apple M4 Mac mini use the
same private kernel, pinned inputs, `read-v1` mode and exact vector checks:

| Prepared process wall time | Original compiler/QuickJS adaptation | Final optimization pass |
| --- | ---: | ---: |
| EJS | 46.97 s | 46.59 s |
| YouTube.js dependency slice | 6.53 s | 6.09 s |

These are single runs including loading/setup, not statistically established
speedups. Coarse EJS compilation time changed from 42 to 39 seconds. Peak tracked
allocation was essentially unchanged. Both backends passed all five exact
vectors, negative controls, zero-live-allocation teardown and filesystem checks.
The same final EJS executable took 47.79 seconds with the older installed kernel
versus 46.59 with the private kernel; this is a whole-kernel comparison, not an
isolated measurement of the read-pin fix. Earlier laptop runs overlapped busy
jobs and must not be used to claim a large before/after speedup.

Implemented changes include balanced C switches/native dispatch, bounded direct
helpers based on longest paths, checkpoints on actual CFG cycles, reconstruction
of fixed local addresses at resume, wide read-view loads, hosted high-bank GEP,
expired-read-pin release, bounded QuickJS frame reuse and binary bit scans.
The remaining cold bottleneck includes continuation save/reload calls and
dynamic global binding: a short M4 profile found these hot in `JS_CallInternal`.
This pass does not eliminate the C16 compatibility/continuation costs.

Final cold YouTube.js preparation still exceeded the 600-second JS deadline
while parsing. It reached 53,831,447 tracked cells (102.7 MiB), freed every
tracked allocation and passed filesystem checks. This interrupted parse does
not establish its eventual memory peak or a successful cold-start path. The
prepared gates pass; the cold gate remains blocked and must continue to fail
the `--full` suite. Full cold EJS was not rerun with the final compiler.

The compact [results receipt](performance-results-2026-10-22.json) retains
input/compiler/engine/executable/library/kernel hashes, timings and validation
counts. Batching continuation transfers and avoiding repeated whole-function
global rebinding are the next profile-backed candidates; they require their
own ABI/lifetime review and acceptance tests.

For this ES6 fixture, the analyzer's parser options create 787,589 syntax
nodes and 1,686,609 objects/arrays. Reproduce the count on the development host
without executing any player code:

```sh
node ports/ytmusic/extraction/inspect-player.mjs
```

## Boundaries and adaptation

`probe.c` embeds QuickJS with no JS filesystem, network, process or module-loader
bindings. Input files are bounded at 8 MiB each. An experimental heap ceiling of
512 MiB on the host / 536,870,912 allocation cells in C16 is a diagnostic cap,
not a measured minimum or an accepted application budget. A JS interrupt and
an outer alarm bound each invocation; this is not a complete hostile-code
sandbox or a production worker protocol.

`fixture-ytjs.ts` uses upstream `JsAnalyzer`, `JsExtractor` and `nsigMatcher`,
then a small adapter around the extracted URL constructor to expose the two
EJS callbacks. Meriyah 7 requires `structuredClone` for plain Identifier and
string Literal nodes; a narrowly checked temporary clone adapter supports
those nodes and is removed before the player executes. This does not add a
general browser API. A host bundle includes test vectors and both fixture
adapters where referenced; it is not a production-size package measurement.

`UPSTREAM.json` and the npm lockfile identify exact inputs. EJS is Unlicense;
YouTube.js and QuickJS are MIT. Preserve upstream license files and per-file
notices, including Meriyah/astring and other bundled dependencies, when creating
redistributable packages. Current fetched source trees retain those notices.
