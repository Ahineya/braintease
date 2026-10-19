# Ripple kernel scheduling and process costs

The main MMU/RVM kernel enables `CONFIG_RIPPLE_SCHED`. FUZIX's process and signal
semantics remain, with port-local runnable/event queues and clock-based scheduling
in `ripple/scheduler.c`. Native Fuse-X also uses these queues and budgets through
its compiler checkpoints; the small Brainfuck profile retains its existing
scheduler. Shared select improvements apply to both full kernels.
No instruction, syscall, executable or filesystem ABI changes
are required.

## State transitions and queues

`process-state.h` supplies `proc_setstate`. On RVM this helper changes process
state, updates `nready`, removes obsolete wait registrations and maintains a
FIFO of ready tasks under disabled interrupts. `nready` includes running and
ready processes; only ready tasks belong to the queue. Other platforms retain
ordinary state assignments and their previous accounting through macros.

The queue uses fixed arrays of slot indices, with slot-plus-one links and zero
sentinels. Selecting the next task does not scan empty, sleeping or stopped
process slots. Repeated ready transitions cannot enqueue a task twice. The
existing batch-task flag skips one queue turn before allowing the task to run.
The queue preserves FIFO fairness instead of repeatedly placing I/O tasks ahead
of CPU-bound processes.

Sleeping tasks register in one of 32 buckets keyed by the complete event pointer.
`wakeup(event)` inspects that bucket and compares exact pointers; collisions
cannot wake unrelated events. Direct wakeups, signals, stop/continue, exit and
slot reuse remove registrations through the same state helper. Noninterruptible
I/O waits retain their event when a signal is queued. The wait transition and
wakeup are protected by the existing IRQ discipline, including the final
readiness check before sleep.

## Slice and wakeup policy

Existing host monotonic samples supply a default 20 ms slice, scaled by the
process's FUZIX priority and bounded to 5–200 ms. A task waking another runnable
process can request rescheduling once the current slice has consumed 5 ms.
Actual task selection remains FIFO. Switching tasks starts a fresh budget.
Native Fuse-X overrides the base slice to 5 ms to retain its existing checkpoint
responsiveness; it uses the same queue and priority policy.

The existing 100,000-instruction timer IRQ remains a scheduling opportunity.
The older instruction-count ceiling remains a progress fallback, including if
clock sampling fails. A pending reschedule is honored at safe user/IRQ and
syscall boundaries; kernel C execution remains nonpreemptive. These are budgets,
not latency guarantees: IRQ spacing, disabled interrupts, JIT compilation and
host scheduling can delay a switch. Guest calendar changes do not alter slices.

Syscall dispatch retains an entry clock sample and the shared syscall exit
sample. Its duplicate outer exit sample is removed. Explicit clock syscalls and
blocking paths still take samples where their contracts require them.

## Fork, memory walks and teardown

Process-slot allocation no longer builds an empty child map that fork would
immediately discard and recreate. Fork constructs the child map once, shares
user pages through COW and copies only the live prefix of the upward-growing
kernel stack, bounded by the saved SP. The child retains a separate kernel stack.

Each address space records a highest-present-page bound for every virtual
heap/data/stack bank. Fork, release, exec preflight and resource reporting visit
those bank prefixes and still check PRESENT on every entry. Holes are supported;
failed-growth rollback and heap shrink trim empty tails, and map replacement or
release clears the bounds. COW copies preserve the mapping's bounds. This changes
bookkeeping, not which addresses are valid or how allocation failure is reported.

RVM and Fuse-X process exit no longer invoke a global filesystem sync. Descriptor closing,
ordinary dirty-buffer eviction, explicit `sync`, synchronous I/O and the shutdown
barrier retain their existing behavior. Exiting a process is not a durability
barrier. The Brainfuck profile retains its previous exit behavior.

## Select registration

Selector notifications return immediately when no selector is active. When
maps are active, wakeup skips empty mask groups and visits set waiter bits.
Normal completion, EINTR and fatal exit use a per-task list of the inode indices
actually registered by that select call, instead of scanning all cached inodes.
Indices live in kernel storage so fatal exit does not follow a discarded syscall
stack. Duplicate descriptors are safe to unregister repeatedly. Timer-only waits
have no inode records to remove. Slot reuse clears the list and its timer.

## Validation and measurement

From the repository root, after building the kernel and relevant probes:

```sh
python3 ports/fuzix/tools/test_scheduler.py
python3 ports/fuzix/tools/test_process_capacity.py --build build/fuzix
python3 ports/fuzix/tools/test_jobs.py --build build/fuzix --engine jit
python3 ports/fuzix/tools/test_select.py --build build/fuzix
python3 ports/fuzix/tools/test_select.py --build build/fuzix --local
python3 ports/fuzix/tools/test_timers.py --build build/fuzix --engine both
python3 ports/fuzix/tools/test.py --build build/fuzix --engine both
python3 ports/fuzix/tools/bench_kernel.py --build build/fuzix --output build/kernel-bench
```

The queue unit test compiles the actual scheduler implementation with a small
host fixture. It checks all 64 slots, FIFO order, duplicate transitions, bucket
collisions, interrupt-state preservation, stop/zombie/reuse cleanup, batch-task
deferral, null events, clock wrap, priority clamps, minimum wakeup slices and
30,000 deterministic state transitions with queue/count
invariants. Guest tests exercise the real kernel: process exhaustion/reuse,
high-slot timers, COW, fork/exec/wait, signals, PTYs, local/network sockets,
preemption and persistent-file recovery.

The optional benchmark is not installed on normal disks. It uses private images
and measures cheap syscalls, clocks, pipe transfers, fork/exit/wait and blocking
parent/child round trips. `--kernel PATH` selects a saved SYS1 baseline without
replacing the built kernel; `--engine interpreter` selects the non-JIT engine.
Use `--probe PREVIOUS_OUTPUT/kernelbench.rxe` for the second run to reuse the
exact compiled workload: independent compiles can change symbol ordering.
Results include kernel/probe hashes, raw samples, medians, total guest instructions
and JIT statistics. Instruction totals include boot, the shell and executable
loading; wall times include compilation and host scheduling. Compare identical
probe hashes and repetition counts and report cold/warm variation.

## Recorded comparison (October 19, 2026)

The saved pre-change kernel and the changed kernel each ran five repetitions
on separate disposable disks, sequentially, with the same RXE probe
(`43332c9edfbf912998367ff190c6a1d1d36a28432e9e0f5346899b92232bb73f`).
The complete run used 2,675,478,577 versus 1,163,706,143 guest instructions:
**56.5% fewer**, including boot, executable loading and shutdown.

Median time per operation across all five repetitions:

| Operation | Before | After |
| --- | ---: | ---: |
| `getpid` | 175 µs | 91 µs |
| monotonic clock query | 200 µs | 147 µs |
| self pipe, one cell | 885 µs | 600 µs |
| self pipe, 512 cells | 1.065 ms | 0.715 ms |
| fork + child exit + wait | 184.2 ms | 19.1 ms |
| blocking parent/child pipe round trip, one cell | 10.170 ms | 1.815 ms |
| blocking parent/child pipe round trip, 512 cells | 3.415 ms | 1.935 ms |

These are observations from a development machine, not general speedup claims.
There is substantial variation: the first `getpid` batch averaged 1.855 ms per
call before and 0.783 ms after, whereas later batches ranged from 94–310 µs and
82–105 µs respectively. Fork samples ranged from 95.1–269.1 ms before and
11.1–39.0 ms after. Exec replaces instruction maps, so later repetitions are
not a guarantee that all guest code remains JIT-compiled. The instruction count
also depends on timer/polling behavior; it is an observed whole-workload count.

Raw samples and reports are saved under `build/kernel-bench-fixed-before` and
`build/kernel-bench-fixed-after` in this checkout. Reproduce the comparison with
`bench_kernel.py --kernel SAVED_SYS1 --repetitions 5`, then run the new kernel
with `--probe FIRST_OUTPUT/kernelbench.rxe --repetitions 5`. No persistent user
disk is involved.

Acceptance checks passed for the actual RVM kernel: all 64 occupied process
slots and repeated recovery; COW and heap boundaries; process/PTY and interactive
job control; TCP/UDP and local readiness; fine deadlines, idle CPU and host pause
recovery; and filesystem persistence/crash recovery. General integration and
coarse clock tests passed with both JIT and interpreter. Native Fuse-X passed
local IPC and fine deadlines after rebuilding the shared code. The small BF
profile rebuilt both CPU images and passed its quick RVM reference suite; a new
long-running BF execution was not part of this change.
