# Native hosted FUZIX: process prototype

This directory contains the first executable milestone toward running FUZIX as
a host application. It proves the process mechanism with a standalone C program;
it does **not** boot the FUZIX kernel yet.

From the repository root:

```sh
cargo build --release -p rcc-driver -p rcc-native-runtime
target/release/rcc compile ports/fuzix/native/process-demo.c \
  --target native -I runtime/include -o ports/fuzix/build/native-process-demo
RCC_NATIVE_HEADLESS=1 ports/fuzix/build/native-process-demo
```

Expected output, with exit status zero:

```text
native: before fork
native: child resumed through three C frames
native: parent reaped child; stack, globals and heap are independent
```

The demo forks from the innermost of three recursive calls. Both continuations
yield and resume, check live 64-bit values and local variables, and return
through the original call chain. The child changes a global, a pointer into its
parent's outer C frame, and a heap allocation. The parent's copies remain
unchanged. It waits for the child and checks its exit status.

## Mechanism

Calling a service declared in `runtime/include/native_process.h` opts the
**whole linked native program** into resumable code generation. Each C function
becomes a native step function. Calls save scalar carriers and a continuation
number in explicit frames, then return to a runtime driver. The driver enters
the callee or resumes the caller. Arithmetic, branches, loops and external
runtime services still execute compiled host machine code. There is no Ripple
instruction decoder, saved native thread stack, host `fork()`, or replay of
earlier instructions.

`rcc_process_fork` clones the active frame chain and logical RAM, including C
locals, globals and heap, plus the allocator state and random seed. The child
receives zero and the parent receives the child's positive PID. A memory image
is moved when switching processes; it is copied only when forking. Fork currently
copies all 32,768,000 bytes of compatibility RAM eagerly. Copy-on-write pages
are a later optimization. The existing logical global/stack/heap limits remain.

`rcc_process_yield` schedules cooperatively. `rcc_process_wait(pid, &status)`
blocks until that direct child exits; `pid == -1` waits for any direct child.
It returns the reaped PID, or `-1` when no matching child exists, and writes the
raw 16-bit exit code. A null status pointer is allowed. This is deliberately a
small API, not POSIX's encoded `waitpid` status/flags/errno contract.
`rcc_process_exit`, an external `exit`, and returning from `main` terminate the
current logical process. The host returns the initial process's exit status
after all remaining processes finish. Orphans finish without leaving zombies;
they are not adopted by an init process in this prototype.

There are at most 16 processes, including unreaped zombies. Fork returns `-1`
at the cap or on exhaustion of positive 16-bit PIDs. PIDs are not reused in a
run; process slots become available after reaping.

Internal function pointers, recursive calls, variadic arguments and floating
values can survive suspension. Taking a runtime binding's address is rejected
in this mode; direct calls to supported runtime services work as before.
Programs without process-service calls retain ordinary native code generation.

## Deliberate boundaries and next milestones

Scheduling is cooperative, and a blocking host call such as `getchar` blocks
the entire driver. Console output, host file handles and devices are shared
host services; they do not yet implement FUZIX's per-process descriptor,
terminal or device semantics. Runtime faults still terminate the host program.
There are no signals, timer preemption, `exec`, kernel/user distinction, or
translation of existing Ripple executable files into native code.

The next port needs a separate hosted platform alongside `ripple/`: kernel
entry and syscall adapters, storage, executable loading, and a nonblocking
console. The first FUZIX goal is a headless shell using the same kernel and
filesystem code. The console should accept a terminal backend independently
of graphics; a network-backed terminal can come before a GUI once the kernel's
networking path is ready. This prototype adds no telnet listener or GUI.

Tests:

```sh
cargo test -p rcc-driver --test native_process
```

These compile and run real executables, covering nested fork, private memory,
live values, indirect/variadic calls, multiple children and grandchildren,
blocking and completed-child waits, process limits and rejected unsupported
runtime callbacks.
