# Injectable effectful Console

`std/console` defines a Console capability and reusable read_line/print/println
effects. A program depends on its environment's capability, rather than directly
calling the global terminal. The same application can run on a real terminal
or a deterministic recording service.

```rfl
import {Console,ConsoleError,println as print_line} from "std/console";
import {Effect} from "std/effect";

fn hello<R:Console>()->Effect<R,ConsoleError,unit> {
    print_line<R>("Hello from an injectable, outrageously pure-looking program!")
}
```

Effects remain ordinary RFL code; this is dependency injection and lazy execution,
not a language-level purity guarantee. These operations accept Strings containing
octet text. Existing std/io::Printable writes directly to global stdout; it is
not silently redirected into a recording Console.

## Application and interpreters

[`examples/console_program.rfl`](examples/console_program.rfl) exports one generic
greeting: write a prompt, read a name, then print a greeting (or handle EOF).
Sequential map2 ensures that reading starts after the prompt completes. Neither
construction nor dropping an unpolled run performs I/O.

- [`examples/console.rfl`](examples/console.rfl) runs the same recipe twice on Terminal.
- [`examples/console-recording.rfl`](examples/console-recording.rfl) runs it twice
  on RecordingConsole with Ada/Ripple input, then prints the captured log.

```sh
target/release/rfl run rfl/examples/console.rfl --target native
target/release/rfl run rfl/examples/console.rfl
target/release/rfl run rfl/examples/console-recording.rfl
```

The two recording runs produce:

```text
Name: Hello, Ada!
Name: Hello, Ripple!
```

## Service contract

Console methods take temporary `&mut self` access and return an **owned lazy
Async**, either a line result or a write result. The effect adapter ends the
environment borrow before awaiting this future. A service must retain whatever
shared/owned state the future needs; it cannot retain the borrowed self reference.
A larger application environment can implement Console by forwarding to a field.
Compute other field-derived arguments before taking its mutable receiver borrow.

`read_line` returns `Ok(Some(text))` without CR/LF, `Ok(None)` at EOF, or a typed
ConsoleError. A final unterminated line is Some before EOF. `print` writes exactly
the supplied octets; `println` adds one LF. Write/read failures, busy streams and
line overflow have distinct error constructors. `map_error` can adapt them into
an application's error channel. Cancellation is separate from typed failure.

## RecordingConsole

`std/console_test::recording_console(lines)` takes complete input lines without
terminators and then reports EOF. It changes its cursor/log only when service
futures are polled. Empty strings are empty lines, distinct from EOF.

Copies intentionally share the same cursor and output log through Shared state.
Use a new constructor call for an independent service. `output()` returns a COW
snapshot of individual writes, preserving embedded newlines; later writes leave
old snapshots unchanged. `consumed()` counts input lines already delivered.
Tests can retain an observer while run owns another handle to the environment.

## Terminal

`std/console_terminal::terminal()` supplies the standalone native/Ripple adapter.
It uses the existing native host input queue or Ripple TTY MMIO. Native input
polling returns immediately; no blocking getchar is performed inside a poll.
Waiting input arms a one-millisecond sleep, and byte processing/output yields
between 64-byte chunks. Use host_reactor or another monotonic millisecond reactor.

Native host input reports EOF. The existing Ripple/Brainfuck TTY register does
not report EOF: without a terminator, reads wait until more bytes arrive or the
future is cancelled. The ISA and device contract are unchanged. The native input
bridge currently treats host read failures as EOF. Guest-OS console adapters
through POSIX/poll remain future work; Terminal is the standalone adapter.
The guest-OS build helper rejects its host/MMIO input dependencies for both
FUZIX and Fuse-X, rather than linking a service that bypasses the guest kernel.

LF, CR and CR/LF terminate lines. A CR remembers the optional LF for the next
read, including when an intervening read is cancelled while waiting. UTF-8 bytes
are preserved with String's existing octet semantics; there is no normalization,
character-level editing or implicit echo. Input buffers accept at most 4096 bytes
before the terminator. Balanced conversion avoids repeatedly copying a growing
string. Overflow consumes the first excessive byte and returns ConsoleLineTooLong;
it does not drain the rest of that line.

Copies of one Terminal share independent read/write guards. A second overlapping
operation of the same kind returns ConsoleBusy; this is fail-fast coordination,
not a queued async mutex. Create one Terminal per physical stream. Independently
constructed services and raw std/io calls do not participate in its guards.

Cancellation releases its stream guard and buffered owners, so another operation
can start. Already-consumed input and already-written output are not rolled back.
Output uses the existing synchronous putchar bridge: chunk yields bound guest
work between calls, not the host's backpressure latency.

## Validation

`tests/console.rs` covers repeated recipes and environments, lazy service futures,
COW log snapshots, EOF/empty/unterminated input, UTF-8 and CR/LF, typed errors,
pending cancellation, overlapping streams, partial output, line limits, and
delegation from an application environment. A native test keeps stdin open and
empty to prove waiting does not block cancellation. RVM checks the unchanged MMIO
path and pending-input behavior.

```sh
cargo test --release -p rfl --test console --jobs 2
python3 rfl/editors/intellij/tests/lsp_applicative_console.py target/release/rfl
```

LSP coverage includes normalized effect/constructor types, callable signatures,
documentation, definitions, formatting and unsupported-instance diagnostics.
The IDEA plugin uses these compiler services; no separate syntax extension is
needed. Update its configured compiler as usual.
