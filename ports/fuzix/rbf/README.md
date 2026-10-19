# FUZIX shell on the Brainfuck CPU

This isolated physical-memory port boots the maintained FUZIX kernel, loads
`/init` from disk and runs a tiny shell. The shell can fork, execute separate
programs and wait for their exit. Both kernel and userspace instructions execute
through the Brainfuck CPU; this is not a host-side shell simulation.

From the repository root:

```sh
scripts/run-fuzix-bf.sh
scripts/run-fuzix-bf.sh --build-only
scripts/run-fuzix-bf.sh --run-only
scripts/run-fuzix-bf.sh --run-only --telemetry
```

The default enables the generic BF JIT and shows `fuzix-bf# ` after boot.
Telemetry is **off by default** so its dots and progress counters do not
interleave with terminal output. Add `--telemetry` for lightweight BF progress.

Try:

```text
uname -a
echo hello > /tmp/hello
echo again >> /tmp/hello
cat /tmp/hello
ls /tmp
cat /BOOT
cd /dev
pwd
ps
status
shutdown
```

`cat`, `ls` and `uname` are separate executables in `/bin`, launched through
FUZIX fork/exec/wait. `cat` and the `uname` command use unchanged upstream utility
sources with a small port libc; `ls` is a minimal directory listing program.
`uname -a` prints `Fuzix 0.5 RCC-BF Ripple`: `Ripple` is the machine ISA and
`RCC-BF` identifies this build. The upstream version field has room for seven
characters plus its terminator.

Built-ins are `help`, `echo`, `pwd`, `cd`, `ps`, `status`, `shutdown` and `exit`.
Paths can be absolute or relative; bare external names resolve in `/bin`.
`status` prints the last external program's exit status. The original shell's
whitespace-separated arguments are retained, with no quoting, expansion,
pipelines or background jobs. `echo` supports a final `> file` or `>> file`.
Lines over 255 characters and commands with more than 23 words are rejected.
The guest tty handles echo, backspace, line kill and canonical EOF (Ctrl-D).
`cat` without arguments reads from that tty until EOF. Ctrl-C stops the host
runner; the launcher restores terminal settings on exit.

Input uses `--tty-stdin` automatically unless an explicit input/display mode is
selected. This also permits pipes:

```sh
printf 'uname -a\nshutdown\n' | scripts/run-fuzix-bf.sh --run-only
scripts/run-fuzix-bf.sh --run-only --input $'ls /dev\nshutdown\n'
```

A shell session has no default timeout. `--timeout SECONDS` adds a wall-time
budget. Other RBF options pass through, including `--no-jit`, `--report`,
execution budgets and checkpoints. `--headless` explicitly selects a run without
automatic stdin attachment; combine it with `--input` or an explicit
`--tty-stdin` as needed.

## Disposable writable disk

Every shell launch, including `--run-only`, copies the built root image into a
fresh private session directory. FUZIX mounts it read/write. Normal writes,
closes and cache eviction use the block driver, including full C16 cell values.
The root image is a raw C16 V3 filesystem with 1,024 blocks (2 MiB physically)
and 32 inodes. No FAT BIOS stage or filesystem downgrade is required.

Shutdown and child exit deliberately skip explicit filesystem sync. Changes
have no durability guarantee and the session disk is removed when the runner
returns, including on interruption. The next launch starts fresh. The built
root template and maintained MMU/RVM user disks are untouched. This launcher
rejects `--disk`.

Build progress streams live using the normal FUZIX launcher's reporter. It
shows compilation, linking, BF export, disk creation and a heartbeat during
quiet steps. Complete diagnostics remain in `build.log`. The default build
directory is `build/fuzix-rbf`; `FUZIX_BF_BUILD` or `--build DIR` overrides it.
Builds in the same directory are serialized. Use `--run-only` when restoring a
checkpoint so its matching image is not rebuilt; the fresh disk still applies.

## Execution and process model

The kernel and programs are compiled to Ripple instructions. The standalone
`.bf` contains a Brainfuck implementation of the CPU and the initial kernel
image. Executables on disk are **SYS1 machine-code data**, rather than separate
expanded Brainfuck files. The guest loader reads them through FUZIX and uses
existing `DEVICE_*` bulk transfers to put code and data onto the CPU's tape.
The Brainfuck CPU then fetches and executes those instructions. Rust supplies
peripherals and tape transfers; it does not execute the Ripple instructions.
The JIT accelerates the underlying Brainfuck interpreter.

This port uses upstream `CONFIG_SINGLETASK`: two process slots, child-first
execution, and parent resumption after child exit. The child has a real PID,
inherits file descriptors with shared offsets, can replace its executable and
returns an exit status through `waitpid`. A further fork while both slots are
occupied fails. A child waiting for console input keeps the parent suspended.
There is no round-robin scheduler or IRQ preemption in this profile.

Banks contain 16,384 C16 cells. The eight physical RAM banks are:

| Bank | Use |
| --- | --- |
| 0 | MMIO |
| 1 | Kernel globals |
| 2 | Active kernel stack |
| 3 | Active user globals |
| 4 | Active user stack |
| 5 | Saved parent user globals |
| 6 | Saved parent user stack |
| 7 | Saved parent kernel stack |

Fork snapshots the parent into banks 5–7. Child execution reuses banks 3–4,
preserving pointers embedded in copied C objects. Parent restoration copies
its user banks back, then switches to a temporary stack in bank 5 before
restoring kernel stack bank 2. This avoids overwriting a live C stack during
the restore.

Kernel code must fit below code bank 10. `/init` occupies banks 10–11; child
programs occupy banks 12–13. The BF export reserves 14 code banks. SYS1 images
use GP 3 and SB 4, with code base 10 for init or 12 for child programs. Each
image may have at most two code banks and one globals bank. Build-time checks
reject conflicting placements and capacity overflow. This ABI is deliberately
distinct from the maintained protected RVM port's relocatable RXE format.

The loader checks regular/executable mode, magic, bank size, physical placement,
entry, capacities and exact section lengths before replacing an image. It reads
code with `readi()` and uploads it with the standard bulk-code device command;
data is transferred into bank 3. User globals and stack are cleared before
entry, and the compiler-generated global initializer runs in userspace.
Arguments occupy the last 1,024 cells of stack bank 4, with at most 24 argv
entries including the program name. Non-empty environments return
`EOPNOTSUPP`; empty environments are accepted. Validation failures preserve the
existing process image; a device failure after replacement begins kills the
child rather than resuming a partially replaced executable.

Code instruction 0 is the synchronous syscall vector; startup begins at
instruction 1. The user wrapper calls that vector with the maintained
`struct request` layout. The gate resolves the request's GP/SB pointer tag,
saves user state, switches to the kernel stack, dispatches, restores state and
returns. Supported calls are exit, open, close, read, write, chdir, dup, getpid,
getppid, execve, getdirent, fork, uname, waitpid and PID1 shutdown. Unsupported
calls return FUZIX `ENOSYS`.

User-memory helpers validate banks and full spans before copies. Larger copies
use the existing bulk device. These checks catch accidental invalid syscall
buffers; they are **not memory protection** against physical-memory code writing
kernel RAM or issuing MMIO itself.

## Boot-only regression profile and limits

The original milestone remains separately available:

```sh
scripts/run-fuzix-bf.sh --run-only --boot-check
scripts/run-fuzix-bf.sh --run-only --boot-check --no-jit
```

It checks the real read-only root mount, PID 1 and `/BOOT` contents, then halts
with result zero. A banner alone is not success. It retains four RAM banks,
a 128-block root image (256 KiB) and a default 60-second timeout. Both profiles
are built by the normal launcher.

| Profile | Kernel | Root template | Default report |
| --- | --- | --- | --- |
| Boot check | `kernel.bin`, `kernel.bf` | `root.img` | `boot.json` |
| Shell | `shell/kernel.bin`, `shell/kernel.bf` | `shell/root.img` | `shell/shell.json` |

Paths are relative to the build directory. Executables are `shell/init.sys`
and `shell/bin/{cat,ls,uname}.sys`. Reports include BF dispatches, guest
instruction starts, tape layout, JIT activity and execution time.

IRQ polling, timer interrupts and preemption are compiled out of both images.
The upstream `Enabling interrupts ... ok.` text refers to inert platform hooks.
The tty uses FUZIX's sleep/wakeup path while `plt_idle()` polls input and uses a
bounded host idle call; no guest clock is advanced. This profile has no MMU,
COW, general multitasking, networking, PTYs, graphics or audio. IRQ-based
preemption and more processes are separate future milestones. The maintained
MMU/RVM port remains separate; `run-fuzix.sh --rbf` still rejects the retired
full non-MMU configuration.

## Validation

After building:

```sh
python3 ports/fuzix/rbf/test_shell.py -v
python3 ports/fuzix/rbf/test_shell.py --rvm-only -v
python3 ports/fuzix/rbf/test_boot.py -v
python3 ports/fuzix/rbf/test_terminal.py
python3 ports/fuzix/rbf/test_session.py
python3 ports/fuzix/rbf/test_image.py
python3 ports/fuzix/rbf/test_build.py
python3 scripts/test_build_progress.py
python3 ports/fuzix/tools/test_launch.py
```

Shell tests compare scripted output between RVM and the BF JIT. They check
fork isolation, PIDs, shared descriptor offsets, repeated wait/reap cycles,
exec arguments and failures, syscall pointer bounds, real block writes with
C16 values, append/truncation, exit status, malformed executables, bank-crossing
code uploads, line editing and live input. Write checks exceed the buffer cache
and inspect host disk bytes to prove writes reach the block device. Each run
uses a disk copy and verifies its template remains unchanged. Launcher tests
cover fresh copies, cleanup, telemetry flags, terminal restoration, build
serialization and live logs. Boot tests retain BF interpreter/JIT parity and
filesystem failure coverage.

Use reports for local timing: build, BF parsing and host setup are separate
costs, and simultaneous builds can materially affect measurements.
