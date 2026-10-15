# RFL lowering decisions

**Status:** Ripple lowering for RFL 0.1; native compatibility target available
**Language spec:** [initial.md](initial.md)  
**IR:** [docs/ir.md](../docs/ir.md)

The native target is documented in [rcc-native/README.md](../rcc-native/README.md). It retains these data layouts and RFL activation records for its first milestone, while generating host machine code and native floating arithmetic. The hardware-stack descriptions below refer to Ripple.

RFL source does not mention banks, fat pointers, the hardware stack, or the width of a machine cell. The compiler and a small runtime hide those limits. `extern "C"` and `unsafe` are the boundary where the Ripple C ABI shows through.

No new IR opcode and no new machine instruction are required. RFL functions are ordinary IR functions lowered by the existing backend.

---

## 1. Calls and recursion

The hardware stack is bank 2. The backend gives every live call a frame there: saved registers, a spill area, and any `alloca`. One frame is small. A recursive RFL call chain is not, and the backend does not grow that bank or perform tail calls.

**RFL-to-RFL calls do not nest on the hardware stack.**

Each monomorphized RFL function is one IR function with the C calling convention:

```text
frame* @fn(frame* self)
```

Its source-level locals, resume point, result slot, caller link, and callee arguments live in a heap activation record. The record is reached by a fat pointer (`mixed` after it is loaded). The function returns the next record for the driver to run. It does not call another RFL function.

The driver is a normal loop:

```text
frame = main_frame
while frame != null:
    frame = frame.code(frame)
```

`null` means `main` has finished. The driver reads `main`'s result from the main record, which it still holds. It does not dereference the null pointer.

A call site:

1. Writes the live locals and a resume index into the current record.
2. Allocates the callee record, with this record as its caller.
3. Returns the callee record to the driver.

The function entry switches on the resume index and continues after the call. Inlining is allowed. A call that remains after inlining uses this protocol.

A tail call drops the current record and returns the callee record with the current caller link. Tail recursion, including `fold`, uses constant activation space. Non-tail recursion, including `map`, uses one record per live call and reclaims it when that call returns.

Activation records come from a bump arena. The arena allocates chunks with `malloc`. Returning to a caller rewinds the bump pointer, so record allocation stays LIFO. A tail call rewinds the current record before allocating the callee, which lets a same-size tail call reuse the space.

Records are not reference-counted. A closure copies captured values into its own environment. That environment is a managed object and may outlive the record.

The hardware stack, at an RFL yield point, holds startup, the driver, and nothing else from the RFL call chain. While an RFL function is running between yields, the backend may also hold that function's spill frame. Source-level objects are not placed in that spill frame. RFL never emits an `alloca` whose size follows a source value.

`extern "C"` calls are direct IR calls. They happen while the active RFL function is on the hardware stack and do not yield. An exported C function may build a frame and enter the driver. If C calls back into RFL during an `extern "C"` call, that entry nests another driver on the hardware stack, once per crossing. Deep recursion inside C still uses bank 2. Pure RFL recursion does not.

---

## 2. Where data lives

| Storage | What goes there |
|---|---|
| Hardware stack, bank 2 | Driver, the active function's spill frame, `extern "C"` callees |
| Global bank, bank 1 | C globals a linked C object actually defines. RFL does not put payloads here |
| Managed heap, banks 5–255 | ADT nodes, strings, closure environments, arrays, and other escaped values |
| Frame arena, chunks from that heap | Activation records |
| Code banks | Function bodies, and the immediates that rebuild static payloads at startup |

The global bank is one bank, and the linker rejects overflow. RFL static payloads are created on the managed heap when the program starts. The startup code stores them from immediate operands in the code image, which already spans code banks. Nullary constructors such as `Nil` and `None` are immediate scalars, not heap objects and not global-bank objects. A constructor with fields is a managed heap object.

Module-level values are initialized by that startup code before `main`. RFL does not emit uninitialized globals. The C startup path does not zero a BSS section.

A managed object is immutable. `let mut` changes which value a local slot holds. Replacing a managed slot releases the old object and retains the new one. Mutable capture is outside 0.1, so a closure cannot build a reference cycle. Reference counts are ordinary cell updates. The compiler inserts retain and release. An immortal object (a static value that lives for the whole run) does not use the counter and is never freed. Release drops fields first, then frees the object.

`malloc` failure, including a frame-arena chunk that cannot be allocated, is a runtime fault.

---

## 3. Representations

Machine cells are 16 bits. RFL types keep the widths in the language spec. The compiler manufactures the narrower ones.

| RFL type | Representation |
|---|---|
| `bool` | `i1`, stored as a cell whose value is 0 or 1 |
| `i8`, `u8` | One cell. After every operation that can set higher bits, mask with `0xFF`. Signed `i8` is sign-extended with an explicit shift pair before a signed compare or a cast to a wider type. The IR `sext` opcode is a move for sub-16-bit values, so RFL does not use it for `i8` |
| `i16` | One cell. Arithmetic wraps as the machine does |
| `u16` | One cell. Comparisons, division, remainder, and shifts use the unsigned IR operations |
| `i32`, `u32` | Two cells, little-endian, the existing wide integer lowering |
| `i64`, `u64` | Four cells, the existing 64-bit lowering |
| `f32`, `f64` | Two and four cells. Operations remain typed `float.*` intrinsics until target lowering. Ripple selects the `__rcc_f32_*` and `__rcc_f64_*` helpers in `libruntime.par`; native selects hardware arithmetic. An IR `add` on a float type is rejected by the backend |
| `char` | Unicode scalar in an `i32` |
| `String` | Managed object: header, byte length, then one UTF-8 octet per cell |
| `unit` | No value. A `unit` function returns `void` |
| Struct or tuple | Value. Small scalar structs are copied as words. A struct that contains a managed field copies the fields and retains those fields |
| ADT | A tag plus a payload. Nullary constructors are immediate. Other constructors are managed heap objects. Pattern matching tests the tag and loads fields |
| Closure | A managed pair: code pointer, environment pointer. The code pointer is the monomorphized function above. The call builds an activation record that holds the environment |
| C function | A Ripple function pointer, instruction offset plus code bank. It has no environment. A closure is not passed where a C function pointer is required |
| Raw pointer | A fat pointer, address cell plus bank cell, and only in `unsafe` or in `extern "C"` signatures |

`i8` and `u8` each occupy a full cell. They wrap at 8 value bits. They are not packed into octets, and they are not C `char`.

C aliases match the Ripple C ABI, not a host C ABI:

| Alias | Width |
|---|---|
| `c_char`, `c_short`, `c_int`, `c_uint` | 16 value bits, one cell. `c_uint` uses unsigned operations |
| `c_long`, `c_size_t` | 32 value bits, two cells. `c_size_t` uses unsigned operations |

A `c"..."` literal is a C string: `i16` code units and a trailing zero, in static storage owned by the C data layout when the literal is passed to C. It is not an RFL `String`. The compiler may place that literal with the same startup-copy rule when it would otherwise pressure the global bank. Native strings do not convert to C pointers.

`repr(C)` structs use the C field list with no implicit padding. Passing one to C follows the existing aggregate convention: the callee receives a pointer to a dedicated object and copies it in. A struct returned from C is a pointer into the callee's hardware frame. RFL copies that object out before any later call. Native RFL calls do not use this convention. Their results move through the activation record.

Loaded managed pointers and pointers returned from calls are `fat(temp, mixed)`. RFL does not dereference `unknown` or `null`. A nullary ADT represented as a null pointer is tested before any load.

Integer casts that change width are explicit in RFL, and the compiler emits the mask, shift, or wide convert itself. Unsigned and signed division stay on the matching IR operations.

---

## 4. Faults

These are runtime faults. They are not `Result` values. The frontend inserts the test before the operation.

| Operation | Test |
|---|---|
| Integer `/` and `%` | Divisor is zero. This covers `i8` through `i64`, signed and unsigned. The machine `DIV` instruction returns 0 on a zero divisor, so the test has to happen in front |
| Indexing a built-in array or other bounds-checked collection | Index is outside the length |
| Allocation | `malloc` returns null |

Float operations follow IEEE arithmetic; Ripple uses software helpers and native uses hardware arithmetic.

A fault calls one runtime entry, `rfl_fail`. It writes a short message with `putchar` and halts with a non-zero status. It does not return. Division by zero in a user function that returns `Result` is ordinary RFL: the function tests the divisor itself and does not use the fault path.

---

## 5. Process entry and exit

The linked C `main` is the RFL driver, not user code. User `main` is an RFL function reached through an activation record.

| User `main` | Status in `RV0` |
|---|---|
| `fn main()` | `0` |
| `fn main() -> i16` | That `i16` |

The full `i16` is the status inside Ripple. `rfl run` on rOS passes that value to the OS exit. On the Rust VM, `rfl run` reports the full `i16` and, when it exits the host process, uses the low 8 bits, because a host process status is 8 bits. RFL programs do not mask the status themselves.

RFL does not call the C `exit` function. That function discards its argument and halts.

---

## 6. What remains finite

A program can use the managed heap until banks 5–255 are exhausted. One allocation may span banks. Running out is `rfl_fail`. There is no separate stack limit and no separate global-data limit for RFL values.

The hardware stack still bounds how deep C can recurse, and how many times C can re-enter RFL without returning. That limit is part of `extern "C"`. It does not apply to RFL calling RFL.

---

## 7. Imports from C

Importing a `.c` file runs the C preprocessor and the C frontend, then links the resulting IR module. Types come from the C typed tree. The IR width alone is not enough: `int` and `unsigned int` are both `i16`. `static` symbols stay internal to the C module and are not imported.

Float code imported from C uses the same typed floating IR and the selected target implementation as RFL.

---

## 8. Out of scope for these decisions

These stay as they are in the language spec: no cycle collector, no threads, no tail-call syntax beyond the lowering rule above, no packing of `i8` into octets, and no change to the C compiler's own stack. A later optimizer may inline more aggressively. It may not move a recursive RFL cycle back onto bank 2.
