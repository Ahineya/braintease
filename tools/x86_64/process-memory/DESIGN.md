# Isolated C16 process memory model

Status: host-tested prototype, 2026-10-20. The x64 platform owner delegated only
this subtree. No production build imports it. It manages sparse backing and
describes mapping plans; it never installs a page table, schedules a process or
changes the existing execution ABI. See the
[coordination agreement](../../../docs/arch/native-memory-coordination.txt) and
[migration direction](../../../docs/arch/native-x64.md).

## Scope and ownership

The platform supplies both the `X64PmSpace` metadata storage and all physical
backing through `X64PmOps`. The module calls no allocator except these callbacks
and has no platform-header dependency. It compiles as freestanding C11.
Metadata, reference ownership and transaction flags are outside page payloads.

The public API is in [process_memory.h](process_memory.h). All exported symbols
use `x64_pm_`. The exposed struct enables caller-owned storage, but its fields
are implementation-private, not binary ABI or public runtime-context offsets.
Never shallow-copy a live space. Current metadata occupies 120,984 bytes per
space on the tested 64-bit host: a fixed frame index and staging bitmap plus
backend/lifecycle state. Payload allocations are sparse even though that index
is fixed. There is no implicit large stack scratch allocation.

Successful `alloc4k` transfers one exclusive, zeroed, aligned 4096-byte payload
to the module. Its ID must be nonzero, 4096-aligned and unique among live
allocations in the backend's memory domain. The `bytes` pointer is a valid
aligned host/kernel alias, not necessarily numerically equal to the ID. Only
the alias is dereferenced. A failed allocation transfers nothing, regardless of
what it wrote into its output record.

The module returns each successful allocation through exactly one `free4k`.
This includes a fresh allocation whose ID/alias alignment or null shape fails
validation. Callbacks must still accept their returned record for disposal.
Exclusive ownership, global identity uniqueness, zero initialization and actual
alias validity remain trusted backend obligations: this model cannot recover
from an allocator returning someone else's memory or an invalid nonnull alias.
The production adapter must validate actual physical frame and alias provenance.

## Logical memory and device boundaries

| Region | Flattened cells | Payload pages | Owner |
| --- | --- | --- | --- |
| Private banks 0 through 239 | `[0, 15360000)` | 7500 | Process model |
| Shared banks 240 through 255 | `[15360000, 16384000)` | 500 | Platform |

Each cell contains all 16 bits. Payload bytes encode cells little-endian, using
two octets per cell independently of host endianness. Read/write operations take
separate validated bank and offset values; they flatten `bank * 64000 + offset`
before identifying a page. An encoded logical bank/offset integer is not a
native byte address. Hardware pages contain 2048 cells and may cross logical
bank boundaries. In particular, bank 0 offset 63999 and bank 1 offset 0 share
page 31. The final private/shared boundary is hardware-page aligned.

Nonempty model reads, writes and zero operations touching logical cells 0
through 31 return `X64_PM_DEVICE`. They implement no MMIO effects. Page-zero
plan entries carry `contains_device_cells`, even if the page has backing for
ordinary later cells. That entry does not authorize direct device access or
choose a future userspace mapping policy. Shared regions are described but
never allocated, cloned, read, written or freed by this model.

## API and error behavior

Create requires zero-initialized caller metadata and nonnull allocator/free
callbacks. It allocates no payload. Creating an already-live space fails.
Release frees private payloads, returns the metadata to reusable state and is
idempotent on a zero/released space. Read/clone/plan on an uncreated or released
space fails. Metadata must remain alive until release completes.

Private operations validate bank `< 240`, offset `< 64000` and the whole span
before accessing payload or caller output. Counts use `uint64_t`; subtraction
bounds reject overflow without adding the count to an unchecked address.
Nonempty reads/writes require a caller buffer. Zero length permits null buffers
and even a device-cell starting address, but still requires a valid private
bank/offset and live space. Shared-bank starts and one-past-bank offsets remain
invalid for zero-length requests. Invalid operations do not partially transfer.

Unbacked cells read as zero. Zeroing holes allocates nothing. Zeroing allocated
storage preserves frame identity, even when an entire payload becomes zero;
automatic sparse reclamation is not implemented. Ordinary successful writes to
an already-backed page likewise preserve its identity.

Writes stage every missing page before modifying any payload. If allocation
fails, all newly staged pages are freed and existing backing, contents and
metadata are restored. Successful staging is followed by infallible cell stores
under the valid-buffer/backend contracts. Caller buffers must not alias model
metadata or owned payloads; callbacks may not modify supplied buffers.

Clone requires distinct live spaces and an empty destination. It may use the
destination's supplied backend. Allocations must still obey the relevant
identity/ownership domain contracts. Every source-backed page receives new
destination backing, while holes remain holes. All allocations are staged
before copying; any failure frees staged pages and leaves the source unchanged
and destination empty. This is eager sparse cloning, not COW. Source and child
can be mutated or released independently afterward.

Errors distinguish argument, lifecycle, range, device access, allocation,
backend shape and busy-state failures. Non-metric output buffers and plan
counts remain untouched on error. Optional metrics are reset per call and
report attempted work, including allocations later rolled back.

## Mapping plan and lifetime

`x64_pm_plan` returns up to `capacity` consecutive descriptors starting at a
hardware-page index. It examines only those entries. Each descriptor identifies
an owned private frame, an unallocated private zero page, or a platform-owned
shared page. Shared/zero entries have ID zero. It returns no payload alias and
does not transfer or duplicate ownership.

Page index 8000 is a valid empty end cursor; larger cursors fail. A zero
capacity allows a null output array. Positive capacity requires an output array
only when entries remain. The caller provides storage for the returned count;
overflow-sized capacity is clamped by the remaining 8000-page model extent.

Plans are borrowed snapshots valid only until the owning space is mutated or
released. They are not PTEs, permission grants, pinning or TLB contracts. A
future live-mapping adapter must retain frame ownership until no active mapping
or user-copy operation can access it. The API does not select an active process,
assign virtual addresses, expose kernel state or create a second run queue.

Calls require external serialization. Participating spaces are marked busy
during allocation/copy/release transactions so callbacks attempting to reenter
their APIs fail. The busy field is not an atomic lock or an IRQ/preemption
mechanism. There is no borrowed payload pointer valid across mutation/release,
and no SMP or asynchronous mutation guarantee.

## Validation and measurements

Run the standalone host suite:

```sh
python3 tools/x86_64/process-memory/test.py
```

Use `--cc` to select a compiler and `--sysroot` or `SDKROOT` when its SDK needs
explicit selection. This checkout's default macOS 27 SDK was incompatible with
the installed linker; validation used:

```sh
python3 tools/x86_64/process-memory/test.py \
  --sysroot /Library/Developer/CommandLineTools/SDKs/MacOSX26.5.sdk
```

The runner first checks a freestanding object build, then executes ten host
test groups with fatal UndefinedBehaviorSanitizer. Fake allocator IDs differ
from payload aliases. It checks zero holes, alignment/shape contracts, private
and shared limits, MMIO rejection, all cell bits, page/bank crossings, maximal
counts, zero-length behavior, callback reentry and output preservation.
Multi-page write and sparse clone transactions fail at every allocation point,
verify byte-for-byte metadata/content rollback and retry successfully. Further
checks cover child isolation, zeroing, release/reuse, a deterministic
100,000-cell oracle and full 7500-page parent/child capacity.

The full private arena has 30,720,000 payload bytes. Eager clone allocates 7500
new frames and copies those bytes. Enumerating the full private/shared plan in
64-entry batches examines 8000 descriptors and copies zero payload bytes. These
are model-work counters, not measured physical context-switch costs, scheduling
latency, hardware page-table performance or a production speedup.

Results and the exact commands are retained in
`build/x86_64-process-memory/result.json` and `host.log`. Temporary objects and
test binaries are removed on success or failure. No guest image is created.

## Production integration gates

The platform owner retains all existing boot/runtime/process/loader/timer and
driver sources. Before any adapter imports this module, agree on physical
allocation budgets, frame/alias validation, virtual-address windows, shared
kernel access, page permissions, TLB and mapping rollback, user-copy pinning,
saved CPU/FP contexts and stacks. Confirm fork/exec/signal ownership and failure
cleanup. Preserve the current checked helper behavior and freestanding read-v1
rejection until independently reviewed implementations exist.

First integrate mapped backing under existing safe checkpoints. Ring-3 helper
placement and true timer preemption follow a separate execution contract. This
host model does not satisfy the later checkpoint-free userspace-loop acceptance
gate, does not alter the shared IRQ stack and does not certify physical Wi-Fi.
