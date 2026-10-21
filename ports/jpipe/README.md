# JavaScript data pipelines

`jpipe` is one C executable embedding the existing QuickJS port. Its aliases
are `jlines`, `jobj`, `jmap`, `jfilter`, `jreduce`, `jsort`, and `jtext`.
The host reference uses pristine upstream QuickJS; native Fuse-X and RVM/Riplix
use the repository's adapted C16 engine. No shell changes or kernel protocols
are required. Nothing is installed or started by building this package.

## Build and test

From the repository root:

```sh
python3 ports/jpipe/build.py --target host
python3 ports/jpipe/tests/test_pipeline.py
python3 ports/jpipe/tests/test_streaming.py
python3 ports/jpipe/build.py --target both
python3 ports/jpipe/tests/test_pipeline.py --target native
python3 ports/jpipe/tests/test_pipeline.py --target ripple
python3 ports/jpipe/tests/test_pipes.py --target native
python3 ports/jpipe/tests/test_pipes.py --target ripple
```

Outputs are under `build/jpipe/{host,native,ripple}`. `--compiler` selects RCC,
`--build` another output root, and `--cc` the host C compiler. Guest compilers
are snapshotted and the build report records the executable hash. Each directory
contains one executable and aliases pointing to it. Guest tests use disposable
images, real shell input redirection, and actual guest symlinks. A separate
three-engine producer/consumer handshake proves that a root JSON array can
flow through real guest pipes before the producer closes the array. Tests never edit
the normal Fuse-X, RVM or x64 disk images.

Use the host commands by adding `build/jpipe/host` to PATH. The native/Ripple
executables require their respective guest systems. `files.mjs` exports an
opt-in image package containing `/bin/jpipe`, the seven relative symlink aliases,
and this reference. It is not imported into default image builds.

For a manual Fuse-X installation, first copy `build/jpipe/native/jpipe.fxe` to
`fuse-x-shared/jpipe` on the host. Then inside the guest:

```sh
cp /mnt/host/jpipe /bin/jpipe
for name in jlines jobj jmap jfilter jreduce jsort jtext; do
    ln -s jpipe /bin/$name
done
```

Create the aliases in the guest filesystem: the host mount deliberately hides
host symlinks. Existing aliases must be removed/replaced explicitly when
installing over an older package. Install the RXE executable the same way on
RVM using its transfer mechanism; FXE and RXE executables are not interchangeable.

## Wire format

The common format is UTF-8 JSON Lines: one complete JSON value per physical
line. JSON string escaping keeps embedded newlines and NUL inside one record.
Objects, arrays, strings, numbers, booleans and null are all records. Stages do
not guess the format from record contents. Blank JSON lines are errors, while
a final line without a line terminator is accepted. Text input accepts LF and
CRLF, preserving other whitespace and empty lines.

| Command | Input | Output | Retained data |
|---|---|---|---|
| `jlines` | Text lines | JSON string records | Current line |
| `jobj` | One JSON document | Root-array elements, or one record for another root value | Current element/value |
| `jmap FUNCTION` | JSON Lines | Function results | Current record and user-held state |
| `jfilter FUNCTION` | JSON Lines | Records with truthy predicate results | Current record and user-held state |
| `jreduce --init JSON FUNCTION` | JSON Lines | One accumulated value at EOF | Accumulator and user-held state |
| `jsort COMPARATOR` | JSON Lines | Stable sorted records after EOF | All sort input plus merge scratch |
| `jtext` | JSON Lines of strings | Raw UTF-8 text, LF after each record | Current string |

`jobj` removes only the root array container; nested arrays remain array
records. An empty root array emits nothing. An object root is one record, not
a stream of its property values. A primitive root is also one record. Extract
nested arrays explicitly with a callback or preprocess the document; there is
no JSONPath selector or flat-map operator in this version.

```sh
ls | jlines | jmap 's => s.toLowerCase()' | jfilter 's => s.includes("test")' | jtext
cat some.json | jobj | jsort '(a,b) => a.index-b.index' | jmap 'o => o.name' | jtext
cat some.json | jobj | jmap 'o => { o.index++; return o; }'
cat records.jsonl | jreduce --init 0 '(total,o) => total+o.size'
printf '"one"\n"two"\n' | jreduce --init '""' '(a,s,i) => a+(i?", ":"")+s' | jtext
```

Text output requires strings; convert numbers explicitly with `x => String(x)`.
Text output may contain embedded newlines or NUL and therefore is not itself a
record-preserving wire format. For arbitrary filenames, a future NUL-delimited
adapter/directory-record producer is preferable to interpreting `ls` output.

## One-process pipelines

`jpipe` accepts ordered stages, avoiding a new engine and JSON reparse between
each operation:

```sh
jpipe --input=json \
  --filter 'o => o.enabled' \
  --sort '(a,b) => a.index-b.index' \
  --map 'o => o.name' \
  --output=text < some.json

jpipe --sort '(a,b) => a-b' \
  --reduce '(sum,n) => sum+n' --init 0 \
  --map 'n => ({total:n})' < numbers.jsonl
```

`--input jsonl|json|text` and `--output jsonl|text` also accept `=VALUE`.
JSON Lines is the default input/output. `-R` selects text input and `-r` text
output, including on aliases. With no stages, `jpipe` validates/transcodes its
input. Stages can repeat and have no fixed count quota. Every reducer requires
its own initial JSON value. Place `--init` immediately before `--reduce`, or
after the reducer it initializes.

Stage indices are local: map/filter receive `(value, index)`; reduce receives
`(accumulator, value, index)`, all starting at zero. Sort comparators receive
`(a,b)` and must return a finite number with consistent ordering. Equal values
retain input order. Functions are evaluated once during setup, then called for
each record. Function expressions, arrow functions, statement bodies, mutation
and closures work:

```sh
jmap '(() => { let count=0; return value => ({value,count:count++}); })()'
```

Standalone commands use separate contexts and serialize values between stages.
A fused pipeline retains JavaScript values and object identity until its final
output. Pipelines of ordinary JSON data have the same intended transformations;
code depending on prototypes, `Date` instances, identity or `toJSON` side effects
can observe this boundary difference.

## Streaming and resource behavior

The reader frames JSON array elements incrementally, handling nested containers,
strings and escapes; QuickJS validates each element. It does not read the whole
array into a string or create a whole-document JS array. A non-array root is
one record and must fit as a whole. The largest individual record and user-held
state still determine memory requirements.

Map/filter/adapters flush completed output records immediately. Kernel pipe
backpressure bounds queued output; no background prefetch queue grows with the
input. Sort is a blocking barrier, currently an in-memory stable merge sort;
reduce is an EOF barrier whose accumulator may itself grow with all input.
Several barriers in a fused pipeline are finalized in stage order. Data can
flow incrementally before a later malformed record or callback fails; output
already emitted is not rolled back.

| Option | Default | Meaning |
|---|---|---|
| `--memory` | 4,194,304 allocation units | QuickJS heap, record buffers and sort vectors/scratch |
| `--max-record` | 1,048,576 logical octets | Individual input and serialized output record limit |
| `--max-polls` | 100,000 | Engine interrupt polls per callback/compile/serialization operation |
| `--max-time-ms` | 1,000 | Monotonic execution deadline per operation |

Allocation units are ordinary bytes on the host and C16 cells (two physical
octets each) on the guests. Executable/code memory, libc buffers, stage-option
storage and OS allocator bookkeeping are additional. The C16 terminal stack
policy retains the port's 48,000-cell JS/C-frame guard. Allocation overflow and
exhaustion fail rather than dropping records. The whole lifetime of a pipeline
is not constrained by one callback deadline; waiting for pipe input is normal.

Stdout contains only records/text. `print` and `console.log/error/warn/info/debug`
write diagnostics to stderr. Failures identify the stage and record where
possible; parse/serialization failures identify the current input/output record.
Exit codes are 0 for success, 1 for JavaScript/I/O/resource failures, and 2 for
invalid command arguments. Ordinary Unix pipes carry data and EOF, not an
upstream exit status: a shell may report only the final command's status. Use
its pipe-failure policy where available, or fused `jpipe` for one status that
covers every stage. Normal SIGINT and SIGPIPE behavior permits Ctrl-C and quiet
cancellation when a downstream consumer closes its pipe.

Wire serialization rejects undefined, functions, symbols, BigInt, non-finite
numbers and cycles, including non-JSON nested values that ordinary JSON.stringify
would silently omit or replace. Standard JSON `toJSON` conversion still applies.
JavaScript callbacks are trusted local code, not a security sandbox; the memory
and execution controls are operational budgets. No filesystem, networking,
process or module host API is exposed by this frontend.

Callbacks are synchronous in this milestone. Returning a Promise, scheduling
Promise jobs or rejecting a Promise produces an explicit error; values are
never silently printed as `[object Promise]`. Timers/async OS operations are
not installed. Future ordered async stages should integrate nonblocking input
and output with the existing QuickJS system reactor, rather than blocking it
inside a synchronous reader.

## Implementation seams and later work

`main.c` handles aliases and options. `records.c` owns incremental framing and
line decoding. `engine.c` owns the QuickJS lifecycle, operators, merge sort,
serialization and diagnostics. `jpipe.h` is the internal reusable seam for other
C frontends; it is not a newly promised stable embedding ABI. The build reuses
the QuickJS adaptation/build machinery through its custom-application input
hook; no second engine or modified upstream source tree is introduced.

Next extensions can add flat-map, nested-document selectors, external merge
sorting, NUL-delimited filenames, and ordered asynchronous callbacks. Disk
spill and async host integration are deliberately separate from this working
streaming core.
