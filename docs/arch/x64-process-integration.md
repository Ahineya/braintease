# x64 process backing: production integration boundary

Status: source audit and proposed integration contract, 2026-10-21; fixture
progress reconciled 2026-10-22. The inactive real-allocator/PTE fixture has passed
on the HP's KVM; see the [recorded result](../../build/x86_64-process-memory-fixture/hp-kvm-result.json)
and [master plan](../x64-master-plan.md). Production mappings, ring 3 and
timer-driven userspace preemption remain unimplemented. This document enables
no execution ABI, scheduler or ownership transfer. Its audit describes the
dated source baseline; recheck production code before integrating the next slice.

This refines [native-x64.md](native-x64.md), the exact subtree agreement in
[native-memory-coordination.txt](native-memory-coordination.txt), and the
[isolated model contract](../../tools/x86_64/process-memory/DESIGN.md). The
platform owner retains existing boot/runtime/driver/test/build integration.
The earlier delegation of `tools/x86_64/process-memory/**` remains intact.

## What the production code actually does

[runtime.c](../../tools/x86_64/boot/runtime.c) embeds one flat `Context.cells`
array of 16,384,000 `uint16_t` cells. It is an active compatibility view, not a
per-process hardware address space. Its private boundary is bank 240; banks
240–255 remain shared C16 kernel storage. The hosted runtime's much larger bank
map does not apply here.

[process.inc](../../tools/x86_64/boot/process.inc) keeps 15,000 private snapshot
entries, each covering 1,024 cells / 2,048 octets. Save scans every dirty flag,
checks each dirty payload for all-zero contents, and copies dirty nonzero
payloads into reference-counted `RamPage` snapshots. Restore scans every pair of
snapshot identities and copies or zeroes only differing pages. Fork initially
shares snapshots; later save detaches them. This is software snapshot COW, not
write-protected hardware COW. Frames and heap metadata are copied separately.

The shared kernel chooses the existing process slot. The runtime's dispatcher
performs requests only after generated native activations return. No native
return address is preserved across a process switch. The timer records ticks
and acknowledges the interrupt; it does not schedule. The checkpoint counter
and pending tick determine when generated code calls the kernel's checkpoint
continuation. Long native helpers remain an independent latency source.

`Context` is a private runtime implementation type. Generated checked code uses
explicit helper imports, rather than a public `Context` layout. The build and
image loader still reject unsupported read-v1 memory-view imports; a mapped
backing change must retain those rejection paths.

## Reproducible measurements and their limits

Run the read-only [layout/work audit](../../tools/x86_64/tests/audit_process_switch.py):

```sh
python3 tools/x86_64/tests/audit_process_switch.py
```

It extracts the actual production declarations and asks Clang for freestanding
`x86_64-unknown-none-elf` layouts. It emits source hashes, exact compiler-derived
sizes and explicitly synthetic work examples. It does not build or run a kernel,
touch the HP, time a switch or activate a mapping. Observed with Apple Clang 17:

| Quantity | Octets / entries |
| --- | ---: |
| `Frame` | 4,672 |
| `Heap` | 98,304 |
| `Process` | 1,414,408; allocation rounds to 346 pages / 1,417,216 octets |
| `Context` | 35,976,032; type alignment only guarantees 8 octets |
| Flat active cells | 32,768,000 |
| Private logical payload | 30,720,000 |
| `RamPage` | 2,056: 8-octet refcount, then 2,048-octet payload |
| Fully populated old private snapshot allocation | 15,000 × 4,096 = 61,440,000 |
| Fully populated prototype private payload allocation | 7,500 × 4,096 = 30,720,000 |
| Prototype `X64PmSpace` metadata | 120,984, in addition to payloads and continuation state |

A successful save/restore at continuation depths `d_old` and `d_new` copies
`(d_old + d_new) * 4672 + 2 * 98304` octets of frame/heap state. Even the
depth-one example is 205,952 octets before RAM payload copying. At depth 256 in
both directions, it is 2,588,672 octets. Mapped cells alone do not remove this.

Under the synthetic full-dirty/nonzero/different-arena case, save copies
30,720,000 payload octets and restore copies/zeroes another 30,720,000. Save also
examines 15,360,000 cell values to detect zeros. Actual work depends on dirty
counts, backing identity and depths; these maxima are not observed workload
traffic or elapsed-time measurements. A clean save still scans 15,000 flags and
restore 15,000 pointer pairs. Prototype full-plan traversal enumerates 8,000
descriptors without payload copies; that is not an actual switch implementation.

Before promoting a live adapter, add production counters for those categories,
allocator work and maximum checkpoint gap, plus TSC timing with interrupts
accounted for. Keep measurement disabled/default quiet outside an acceptance
build. Compare the same binaries/workloads and report host, QEMU and physical
measurements separately.

## Ownership and lifetime contract

| Object | Sole owner | Lifetime / release condition |
| --- | --- | --- |
| Physical RAM allocation | Platform allocator in `boot/kernel.c` | Backend returns an exclusive aligned zeroed page; only its matching owner frees it |
| Private backing payloads | One process backing space | From successful callback transfer until all mapping leases/borrows are retired, then exactly one `free4k` |
| `X64PmSpace` index/staging state | Platform-created process metadata | Created zeroed, externally serialized, never shallow-copied while live |
| Process continuation frames, heap and jump records | Existing process runtime | Remain ordinary separately owned metadata; not implicitly covered by a cell mapping |
| Shared banks 240–255 | Platform/shared kernel | Never cloned or freed by private-space release |
| Candidate page tables and bookkeeping | Platform mapping transaction | Staged privately; rollback frees every staged page; published tables remain owned until unlinked and TLB-safe |
| Device/framebuffer mappings and DMA buffers | Existing driver/platform ownership | Never included in process plans, private clone/free, or user-copy grants |
| Image code, templates, function tokens | Existing image module ownership | Resident while a live/staged process owns them; release still goes through `images_collect` |

The initial backend can return `id = page_alloc()` and `bytes =` its validated
kernel identity alias. That equality is a platform fact, never a model assumption.
`page_alloc` already zeroes 4 KiB allocations. Validate the return belongs to
this adapter's live allocation ledger, is below `PHYSICAL_LIMIT`, is aligned,
has an identity alias, and cannot overlap kernel-reserved or device storage.
The callback's zero return transfers no ownership. If success returns a malformed
record, disposal must still work, as the model contract requires.

Do not use `mapping_page_alloc` for releasable process tables. That existing
private function permanently transfers device/framebuffer table pages out of
the releasable allocation set and contributes to `page_mapping_owned()`.
Counting process leaks as permanent kernel mappings would hide them from the
shutdown balance. Process mapping tables need ordinary accounted allocations,
an explicit owner and teardown. Retain the existing late-BAR accounting fix.

The current model's plans borrow identities only. They are not pins. All model
calls require external serialization, and their busy fields are not IRQ/SMP
locks. Production needs an explicit mapping lease: while published, topology
changes, release and allocator reuse are forbidden; payload mutation follows
the accepted writable mapping contract. That lease is an API review item, not
an existing capability of `x64_pm_plan`.

## Actual mismatches to resolve before activating mappings

1. **Snapshot shape:** a current `RamPage` has an eight-octet metadata prefix and
   half a hardware page of cells. Two snapshot pointers are not one contiguous
   hardware payload. Never cast these objects into PTE frame addresses.
2. **Active view:** `Context.cells` is an inline array with no 4 KiB alignment
   guarantee, inside a static kernel allocation. Remapping its pages in place
   can overlap adjacent runtime metadata and break its native identity alias.
   Use a separately reserved aligned virtual window and an explicitly initialized
   runtime cell-view pointer in a later platform-owned change. Audit every
   `c->cells` access and `sizeof` use; do not assume a global textual replacement
   preserves initialization or ownership.
3. **Sparse holes:** a read-only shared zero frame cannot satisfy existing direct
   `memcpy`, `memset` and per-cell writes. Writable sharing would corrupt other
   processes. Choose explicit materialization before all stores or a tested
   fault policy. A simple first live baseline may fully materialize private
   pages before publication, but then budgets and fork costs must reflect that.
4. **Fork:** the prototype eagerly clones each backed 4 KiB payload. Production
   currently increments snapshot references. A naive replacement can increase
   fork latency and committed memory even while reducing ordinary switch copies.
   Stage an eager baseline with an explicit budget; hardware COW is a later
   fault/permission/ownership change.
5. **MMIO at cells 0–31:** hardware protection cannot isolate 64 octets from the
   ordinary cells in page zero. Keep checked helper MMIO effects. A frame in
   the page-zero plan does not grant user access to devices.
6. **Direct runtime borrows:** `root.inc`, `network.inc`, Wi-Fi/entropy mailboxes,
   string/memory helpers and `images.inc` retain native pointers into the active
   view during a synchronous operation. A window must not change underneath
   them. Current checkpoint serialization makes this manageable; arbitrary
   IRQ switching does not.
7. **Failure semantics:** the model stages allocation before mutation. Current
   `process_save` reconciles pages incrementally and can fail partway through;
   switch then stops fatally, while fork reports failure with the active view
   still present. A new mapping commit must be allocation-free and must not
   replace a working view after a failed preparation.
8. **Shared state and image lifetime:** memory selection must preserve shared
   kernel cells and the image ownership scans. It cannot introduce an independent
   process list or free code while a continuation still names its function token.

Logical pointers remain validated bank/offset values. Flatten with
`bank * 64000 + offset` before multiplying by two to obtain payload octets.
A hardware page holds 2,048 cells and can straddle a logical bank boundary;
bank 0 offset 63,999 and bank 1 offset 0 share page 31. The private/shared
boundary is page aligned. No native VA or physical ID becomes a C16 pointer.

## Page-table and commit contract for the first live milestone

Initially retain the one boot CPU and one existing kernel scheduler. A fixed
supervisor virtual window can select the active C16 backing under today's
checkpoints without claiming per-process CR3 isolation. The final VA reservation
must be agreed with the loader/device mapper before implementation; it must be
canonical, page-aligned and disjoint from identity RAM, high BARs, kernel/stack
guards and firmware-owned ranges. Do not silently repurpose an identity region.

The platform constructs PTEs from validated frame IDs, not borrowed aliases or
encoded logical pointers. Private and shared mappings have distinct provenance.
The first stage grants no user bit, no global bit on private mappings and no
device/PAT policy changes. Data should be non-executable once CPUID support and
`EFER.NXE` enablement are explicitly established; current long-mode validation
does not establish NX enablement. Ring-3/code permissions require a later gate.

Preparation holds the process/backing lease, enumerates bounded plan batches,
validates every frame and shared-region source, allocates every necessary table,
and records old mapping ownership without touching the live root. Allocation,
invalid plan, overlap, budget failure or stale generation releases all staged
resources and leaves the running view, process selection and counters unchanged.

Commit happens after the current generated activation has returned, with no
outstanding helper borrow. Disable interrupts only for the bounded publication
and invalidation step; the ISR still does not schedule. Publish the prepared
window tables, perform a compiler/memory barrier, invalidate replaced translations
and paging-structure caches, then update active selection and restore IF. For
the first non-PCID-optimized implementation use a full flushing CR3 reload with
private mappings non-global, rather than assuming an ordinary C store flushes
TLBs. Invalidation must complete before old tables/backing can be reclaimed.
Do not apply the device mapper's cache-transition/WBINVD protocol to ordinary
write-back RAM aliases without a separate reason.

After publication, selection owns a lease until the next safe replacement.
Discard unlinks the old process, retires mappings, invalidates translations and
only then releases its pages/images. Staged `exec` keeps old state intact until
all replacement memory and continuation metadata are ready. No async driver
callback may retain a window alias across this boundary; native DMA remains in
driver-owned physical buffers. User-copy pinning/cancel rules come before
future blocking native syscalls or ring-3 preemption.

## Accepted inactive platform fixture

The **inactive platform backing/plan adapter fixture** is implemented and has
passed on the HP's KVM. It exercises the first platform integration seam without
changing the default process representation. Retain this scope as a regression
checklist:

1. Wrap the real `page_alloc/page_free` with an adapter allocation ledger and
   per-space page budget. Use the existing isolated model APIs unchanged.
2. Create two independent spaces, exercise private values (including high bits
   and page/bank edges), clone, and enumerate plans in bounded batches. Shared
   descriptors must resolve only through a separately supplied platform region;
   page-zero device markers survive validation.
3. Produce a staged 4 KiB PTE image in private table pages and validate it through
   a software page walk. Its root is never installed into CR3 or linked into
   the running root. Hole entries remain explicitly absent in this fixture.
4. Inject failure at every adapter allocation/table-staging point and test bad
   frame provenance, duplicate IDs, invalid aliases, overlaps and stale plan
   generation. All paths return allocator availability to baseline and leave
   the running C16 sentinel, guards, driver maps and process selection intact.
5. Release both spaces/tables, run the existing process/shell/timer/storage
   regressions and verify the unrelaxed shutdown ownership guard. Keep the
   fixture disabled in the normal image.

This acceptance validates the allocator/PTE construction seam using real pages
from the guest platform allocator. It does **not** measure switching, change
the kernel's run queue, run apps at ring 3 or test AX211 DMA concurrency. Host
model results remain host results; QEMU/KVM on the HP does not certify physical
device behavior or active production mappings.

## Next slice: mapping leases and checkpoint selection

The next bounded slice needs a reviewed materialization/mapping-lease contract,
the reserved window, per-switch instrumentation and checkpoint-only selection.
Acceptance then requires both values/continuations and zero **ordinary switch
payload copies**, plus fork/exec/failure/reclamation/blocked-I/O regressions and
unchanged device mappings. Remaining frame/heap copies must be reported, not
hidden by the payload-copy success criterion.

## Ring 3 and timer preemption follow this boundary

The current GDT has only kernel code/data and a TSS; no user selectors or
syscall/trap ABI are established. Every current CPU exception is fatal. The
IRQ GPR/512-octet FXSAVE state lives temporarily on one shared IST2 stack and
is immediately restored. Retaining its address as a parked process context
would allow the next IRQ to overwrite it.

Before preempting applications, separate checked userspace helpers from
privileged services; create process-owned native stack, GPR/return and x87/SSE2
state; add user access permissions and checked copy boundaries; and contain
user faults without stopping the shell. Keep kernel/runtime critical sections
nonpreemptible initially, with bounded deferred switching. Maintain one kernel
process table and run queue. A checkpoint-free ring-3 busy loop beside a
responsive C16 shell, FP-state tests, blocked I/O, signals, fork/exec and exact
reclamation are the execution gate. Ring-0 fixture success is not that gate.

LP64/static ELF64, compiler target work and native kernel migration remain
separate milestones in the [native migration plan](native-x64.md). The accepted
filesystem direction is to adopt an established filesystem; neither this
adapter nor a new native C layout changes disk encoding or settles native file
octet/C16-cell semantics. SMP follows working single-CPU isolation/scheduling
and explicit shared-state/TLB/device synchronization.
