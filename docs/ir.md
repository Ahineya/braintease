# Textual IR

This is the text format of the Ripple compiler's intermediate representation. A language frontend prints a module; `rcc` parses it back into the same in-memory `Module` and lowers it to Ripple assembly. The printer and parser live in `rcc-ir` (`emit_module`, `parse_module`).

The format is lossless. Every field on the module, including ones lowering does not currently read, survives a round trip. Names, types, values, instructions, block lists, and optional metadata are spelled out. There is no separate "debug dump" syntax.

Machine details that this IR targets (16-bit cells, fat pointers, the C ABI) are in [`arch/memory.md`](arch/memory.md) and [`arch/abi.md`](arch/abi.md). `--trace` still writes a serde JSON dump, `filename.ir.json`, described in [`compiler-trace-formats.md`](compiler-trace-formats.md). That JSON is not this format, and `rcc compile` does not read it.

## Using it

Emit text from C and stop:

```bash
rcc compile hello.c --emit-ir
rcc compile hello.c --save-ir          # hello.ir next to the source
rcc compile hello.c --save-ir --ir-output out/hello.ir
```

Lower a text file. The input must use the `.ir` extension. `generate-asm` reads text IR regardless of extension.

```bash
rcc compile hello.ir -o hello.asm
rcc generate-asm -i hello.ir -o hello.asm
rcc generate-asm -i hello.ir --bank-size 64000
```

From Rust, the same module type is shared by frontends and the backend:

```rust
let module = rcc_ir::parse_module(&text)?;
let text = rcc_ir::emit_module(&module); // also format!("{}", module)
```

Parse errors look like `12:5: expected '{'`. A failure is an error. The parser does not guess a module and continue.

Emitting a module prints items in this order: type entries sorted by name, then `internal function` entries, then globals, then functions. A hand-written file may use any order. Parsing keeps each group's source order, and a second emit sorts types and regroups the four kinds.

`;` starts a comment that runs to the end of the line. A `;` inside a quoted string is part of the string.

## Tutorial 1: A constant

C `int` on this target is `i16` (one 16-bit cell). The first block in the function is the entry.

```text
module fortytwo {
  function i16 @main() {
    block 0:
      ret 42
  }
}
```

```bash
rcc compile fortytwo.ir -o fortytwo.asm
```

`ret 42` returns the constant. `ret void` returns nothing. The value does not repeat a type; the function's return type is on the `function` line.

## Tutorial 2: Parameters, locals, and arithmetic

A parameter is a temporary. Its number is explicit and can be any `u32`, not only `0, 1, 2, …`.

An `alloca` reserves stack cells. Its result temporary is the address. The bank is not stored on that temporary, so a later instruction wraps it: `fat(%2, stack)`. The C frontend does this for every local.

```text
module arith {
  function i16 @"add"(i16 %0, i16 %1) {
    block 0:
      %2 = alloca i16 -> i16*
      store i16 %0, fat(%2, stack)
      %3 = alloca i16 -> i16*
      store i16 %1, fat(%3, stack)
      %4 = load i16, fat(%2, stack)
      %5 = load i16, fat(%3, stack)
      %6 = add i16 %4, %5
      ret %6
  }
  function i16 @main() {
    block 1:
      %0 = call i16 global @"add"(1, 2)
      ret %0
  }
}
```

`add` is quoted because `add` is an opcode. `@main` needs no quotes. See [Names](#names).

`alloca i16 -> i16*` means "one `i16`, and the result temporary has pointer type `i16*`". A counted allocation is `alloca i16, 4 -> i16*`. The type after `->` is stored as written. Lowering uses the allocated type and the count for the frame size.

`call i16 global @"add"(1, 2)` is a direct call. `global @name` and `func @name` are different values, and both lower as a call to the label `name`. The C frontend emits direct calls as `global`.

## Tutorial 3: Branches

Comparisons are ordinary binary instructions. Their result type is written out; the C frontend uses `i16` here, not a separate boolean type.

```text
module pick {
  @g = external i16 = 1
  @s = internal i16 = 2
  @e = declaration i16
  function i16 @pick(i16 %0) {
    block 0:
      %1 = alloca i16 -> i16*
      store i16 %0, fat(%1, stack)
      %2 = alloca i16 -> i16*
      %3 = load i16, fat(%1, stack)
      %4 = ne i16 %3, 0
      br %4, label 1, label 2
    block 1:
      %5 = load i16, global @g
      store i16 %5, fat(%2, stack)
      br label 3
    block 2:
      %6 = load i16, global @s
      store i16 %6, fat(%2, stack)
      br label 3
    block 3:
      %7 = load i16, fat(%2, stack)
      %8 = load i16, global @e
      %9 = add i16 %7, %8
      ret %9
  }
}
```

`br %4, label 1, label 2` goes to block 1 when the condition is true and block 2 otherwise. `br label 3` is unconditional. Block ids are the numbers in `label N`. They are not required to be dense, and they are not the position of the block in the function. The entry is whichever block is written first.

`@s = internal` is a definition with internal linkage (C `static`). `@e = declaration` is an `extern` declaration: the name is referenced, and this module does not allocate storage for it. A global used as a pointer operand can be the value `global @g` directly. A pointer *initializer* uses `fat`, shown in the next tutorial.

The same join can be a `phi` instead of a store to `%2`:

```text
module phi {
  function i16 @main() {
    block 0:
      br 1, label 1, label 2
    block 1:
      br label 3
    block 2:
      br label 3
    block 3:
      %0 = phi i16 [1, label 1], [2, label 2]
      ret %0
  }
}
```

Each incoming pair is a value and the block it comes from. Order is significant. Lowering rebuilds the CFG from terminators, so a `phi` still needs a real branch from those blocks.

## Tutorial 4: Globals and strings

C `char` is 16 bits, so a string is an array of `i16` cells, including the trailing 0. A pointer global's initializer is a fat pointer: the address value, plus the bank tag `global`.

```text
module greet {
  @__str_0 = internal constant [3 x i16] = [104, 105, 0]
  @hello = external i16* = fat(global @__str_0, global)
  function i16 @main() {
    block 0:
      %0 = load i16*, global @hello
      %1 = getelementptr i16*, i16 fat(%0, mixed), 1
      %2 = load i16, fat(%1, mixed)
      ret %2
  }
}
```

`[104, 105, 0]` is a constant array. Only integers are allowed inside `[]`. Mixed values, including pointers, use `{ ... }` instead.

`getelementptr` without `field` scales the index by the size of the source element type (`i16` here, one cell). The result type is the type of the pointer that temporary represents (`i16*`). The load records the runtime bank on `%0`. The next use writes `fat(%0, mixed)`, which means "use the bank that instruction already recorded." Use `stack`, `global`, or `heap(n)` when the frontend knows the region itself.

`constant` records that the C object was const. Lowering currently allocates and initializes the global either way; the flag is still stored and round-trips.

An address with a constant displacement is `offset`:

```text
@slot = external i16* = fat(offset(global @__str_0, 1), global)
```

`offset(global @__str_0, 1)` is "one cell past the start of `__str_0`". The bank stays on the enclosing `fat`. The symbol stays a symbol so the linker can relocate it.

## Tutorial 5: Struct fields

Struct layout is the field list, in order, with no implicit padding. A field GEP uses a constant word offset and the `field` marker. Without `field`, that same `1` would be scaled by the size of the whole struct.

```text
module point {
  function i16 @main() {
    block 0:
      %0 = alloca struct Point { i16, i16 } -> struct Point { i16, i16 }*
      %1 = getelementptr i16, struct Point { i16, i16 } fat(%0, stack), 1 field
      store i16 7, fat(%1, mixed)
      %2 = load i16, fat(%1, mixed)
      ret %2
  }
}
```

The struct name `Point` is a label on that type occurrence. It does not declare an alias. Writing `Point` later, without `struct Point { ... }`, is a parse error. A `type` entry, documented below, stores a named type on the module and also does not create an alias.

A nonzero field offset leaves the bank as `mixed` in C-generated IR, because a large struct can cross a bank. Offset 0 keeps the base bank.

## Tutorial 6: Calls, indirect calls, and varargs

```text
module calls {
  @fmt = internal constant [1 x i16] = [0]
  function external vararg i16 @printf(i16* %0) symbol 1 loc "libc.h" 1 1 {
  }
  function i16 @main() {
    block 0:
      %0 = call i16 global @printf(global @fmt) stack 1
      %1 = alloca i16* -> i16**
      store i16* func @printf, fat(%1, stack)
      %2 = load i16*, fat(%1, stack)
      %3 = call i16 fat(%2, mixed)(global @fmt)
      ret %3
  }
}
```

`function external` is a declaration. Lowering does not emit a body. `vararg` marks the function as variadic. `stack 1` on the call forces arguments at index 1 and above onto the stack; argument 0 still uses the normal register ABI. Omit `stack` when every argument follows the normal ABI.

`func @printf` is a function value. Stored through a pointer and loaded back, it becomes an ordinary temporary. Calling `fat(%2, mixed)` is an indirect call: the value is not `global` or `func`, so lowering uses the fat pointer as a code address and the bank recorded by the load.

`symbol` and `loc` are optional. They round-trip and are not read when lowering. `symbol 0` is a real id, distinct from omitting `symbol`.

A C `static` function is also listed, so the linker can rename it per translation unit. Globals do not need this line. Linkage `internal` or `private` is enough for them.

```text
module helper {
  internal function @helper
  function i16 @helper() {
    block 0:
      ret 1
  }
  function i16 @main() {
    block 1:
      %0 = call i16 global @helper()
      ret %0
  }
}
```

## Tutorial 7: Inline assembly and comments

```text
module asm {
  function i16 @main() {
    block 0:
      %0 = alloca i16 -> i16*
      comment "load the answer"
      asm "LI %0, 42" outs "=r"(fat(%0, stack))
      %1 = load i16, fat(%0, stack)
      ret %1
  }
}
```

`comment "..."` is an instruction. It becomes an assembly comment. A `;` line comment is discarded by the parser and is not an instruction.

`asm` takes a quoted assembly string, then any of these sections: `outs`, `ins`, `clobbers`. Each operand is a quoted constraint, a parenthesized value, and an optional `tie N` (the index of the operand it is tied to). Clobbers are quoted strings. Empty sections are omitted.

```text
asm "ADD %0, %1, %2" outs "=r"(%0) tie 0 ins "r"(%1), "r"(%2) clobbers "t0"
```

## Tutorial 8: Another frontend

A frontend for another language only has to print this text. It does not link the C lexer or parser.

1. Choose types using the [size table](#types). On this machine a C byte is one cell, so a byte-oriented language still has to decide how it packs octets.
2. Give every pointer you dereference a bank lowering can materialize: `stack` for allocas, `global` for data symbols, `heap(n)` for a known heap bank, or `mixed` on a temporary whose defining load or call already recorded a bank. `unknown` and `null` are legal tags in the text. Dereferencing them aborts lowering.
3. End every block with `ret` or `br`, unless you intend the [fall-through rule](#what-lowering-checks).
4. Put the entry block first.
5. Use `global @name` or `func @name` for a direct call. Any other call value is an indirect call and must be a fat pointer.
6. Spell struct and array types at each use. Do not rely on `type` entries as aliases.
7. Mark translation-unit-local functions with `internal function`, and local globals with linkage `internal` or `private`.
8. Check the file with `rcc compile file.ir -o file.asm`. A parse error names the line and column. A lowering error means the module parsed and the backend rejected it.

The Rust library is the other path: build a `rcc_ir::ir::Module` and call `rcc_backend::lower_module_to_assembly_with_options`. Text is the interchange format for a frontend that is not Rust, and for inspecting or editing a module between those stages.

## Lexical syntax

Input is a sequence of tokens. Spaces, tabs, and carriage returns are ignored. A newline separates instructions. Newlines inside `()`, `[]`, and `{}` are ordinary whitespace, so a call or a struct type may wrap.

| Token | Form |
|-------|------|
| Name | A letter, `_`, or `$`, then letters, digits, `_`, `.`, or `$`. `demo.c` is one name. |
| String | `"..."`. Escapes: `\\`, `\"`, `\n`, `\r`, `\t`, `\u{hex}`. |
| Integer | Decimal digits. No hex, no underscores. The magnitude must fit in `u64`. |
| Arrow | `->` |
| Ellipsis | `...` |
| Punctuation | `@ % * ( ) { } [ ] , : =` |
| Minus | `-`, used for negative numbers. `-9223372036854775808` is `i64::MIN`. |

A name that is a [keyword](#keywords), or that is not a bare name (`"has space"`, `"123"`, `"a/b"`), is written as a string. `@` then a name or a string forms `@main` or `@"add"`.

`;` comments are stripped before tokenization. They cannot replace an instruction.

## Grammar

```text
module     := "module" name "{" item* "}"
item       := type_def | internal | global | function

type_def   := "type" name "=" type
internal   := "internal" "function" at_name
global     := at_name "=" linkage ["constant"] type ["=" value] ["symbol" int]
function   := "function" ["external"] ["vararg"] type at_name
              "(" param ("," param)* ")"
              ["symbol" int] ["loc" name int int]
              "{" block* "}"
param      := type "%" int

block      := "block" int ["loop"] ["preds" int_list] ["succs" int_list] ":"
              instruction*
int_list   := int ("," int)*

instruction := result "=" opcode ...
             | opcode ...          ; store, ret, br, debugloc, asm, comment,
                                    ; and result-less call / intrinsic
```

`int` in the grammar is a `u32` except where a value or a type size says otherwise. Array sizes are `u64`. Constants and `offset` displacements are `i64`. Heap bank numbers are `u16`. `stack N` and `tie N` are `usize`.

Trailing commas are errors. So is a second instruction on the same line, and any token left over at the end of an instruction.

## Keywords

These words are bare names only in the position where the grammar expects them. As a module, global, function, struct, or file name they must be quoted.

```text
module type internal function external private declaration constant symbol loc
vararg block loop preds succs
void i1 i8 i16 i32 i64 f32 f64 label struct packed x
global func fat offset undef heap stack unknown mixed null
add sub mul sdiv udiv srem urem and or xor shl lshr ashr
eq ne slt sle sgt sge ult ule ugt uge
not neg zext sext trunc ptrtolinear ptrtoint inttoptr
load store volatile getelementptr field alloca call ret br
phi cast select intrinsic debugloc asm outs ins clobbers tie comment to
```

`x` is reserved because array types are written `[4 x i16]`.

## Types

A type is a primary type followed by zero or more suffixes. `*` wraps the type so far in a pointer. `(...)` makes a function type whose return type is the type so far. `...` as the last parameter marks a variadic function type.

```text
i16*              pointer to i16
i16**             pointer to pointer
[4 x i16]         array of 4 cells
[4 x i16]*        pointer to that array
[4 x i16*]        array of pointers
i16(i16, i16*)    function
i16*(i16)         function returning a pointer
i16(i16)*         pointer to a function
i16(...)          variadic function
struct { i16, i16 }
struct Point { i16, i16 }
packed struct "i16" { i8 }
label
```

| Primary | Words (cells) | Typical C type |
|---------|---------------|----------------|
| `void` | none | `void` |
| `i1` | 1 | `_Bool` (one value bit) |
| `i8` | 1 | internal 8-bit integer; C `char` is `i16` |
| `i16` | 1 | `char`, `short`, `int` |
| `i32` | 2 | `long` |
| `i64` | 4 | `long long` |
| `f32` | 2 | `float` |
| `f64` | 4 | `double` |
| `T*` | 2 | pointer (address cell + bank cell) |
| `[N x T]` | N × size(T) | array |
| `struct { ... }` | sum of the fields | struct or union field list as laid out by the frontend |
| function, `label` | none | not a memory object |

`packed` is recorded on the struct. Size is still the sum of the field sizes; the format does not insert padding. The frontend is responsible for the field list it wants in memory.

Empty structs and empty parameter lists are allowed: `struct { }`, `function i16 @main()`.

### Type entries

```text
type Point = struct Point { i16, i16 }
type "i16" = packed struct "i16" { i8 }
```

This stores the module's type map. The key and the struct's own name are independent. Lowering does not look names up in this map. An instruction that uses the struct repeats `struct Point { i16, i16 }`.

## Values

| Syntax | Meaning |
|--------|---------|
| `%12` | Temporary. The id is a `u32`. |
| `42`, `-1`, `9223372036854775807` | `i64` constant. |
| `undef` | Undefined value. Distinct from omitting an initializer. |
| `global @g` | The data symbol `g`. |
| `func @f` | The function symbol `f`. |
| `fat(%1, stack)` | Address value plus a bank tag. |
| `offset(global @g, -4)` | `g` displaced by `-4` cells. The bank lives on the enclosing `fat`, when there is one. |
| `[104, 105, 0]` | Constant array of `i64`s. `[]` is empty. Not a place for pointers. |
| `{1, global @g, undef}` | Aggregate of values. `{}` is empty. |

`global @f` and `func @f` compare as different values. Both lower as a direct call when they are the callee. A callee that is a `fat` or a temporary is an indirect call.

### Banks

Written only inside `fat(...)` and as `heap(n)`.

| Syntax | Meaning |
|--------|---------|
| `global` | Data / read-only data. Runtime bank tag −1, register `GP`. |
| `stack` | Frame and `alloca`. Tag −2, register `SB`. |
| `heap(5)` | Heap bank 5. The number is a `u16`. |
| `unknown` | Bank not known at this value. Legal in the text. Dereferencing it aborts lowering. |
| `mixed` | Use the bank a previous instruction recorded on this temporary, or a pointer whose paths do not share one region. |
| `null` | Null pointer. Dereferencing it aborts lowering. |

## Globals

```text
@g = external i16 = 1
@g = external constant i16 = 1 symbol 3
@s = internal i16 = 2
@p = private i16*
@e = declaration i16
@u = external i16 = undef
@hello = external i16* = fat(global @__str_0, global)
```

| Linkage | Meaning |
|---------|---------|
| `external` | Defined here and visible to the linker. |
| `internal` | Defined here. The linker renames it so another object can use the same source name. C `static` storage. |
| `private` | Same rename as `internal`. |
| `declaration` | Name only. No storage and no initializer image in this module. C `extern`. |

The initializer is optional. Omitting it is not the same as `= undef`.

`constant`, when present, sets the global's const flag. `symbol N` stores an optional `u32`. Neither is consulted by lowering today.

## Functions and blocks

```text
function external vararg i16 @printf(i16* %0) symbol 1 loc "libc.h" 1 1 {
  block 0 loop preds 2, 0 succs 1:
    ret 0
}
```

| Piece | Meaning |
|-------|---------|
| `external` | Declaration or otherwise not lowered. The body is still stored. |
| `vararg` | Variadic function. |
| `i16` | Return type. |
| `@printf(i16* %0)` | Name and parameters. Each parameter is a type and a temporary id. |
| `symbol 1` | Optional symbol id. |
| `loc "libc.h" 1 1` | Optional source file, line, and column. The file is a name or a string. |
| `loop` | Sets the block's loop-condition flag. Preserved. Lowering currently only logs it. |
| `preds 2, 0` | Predecessor block ids, in this order. |
| `succs 1` | Successor block ids, in this order. |

`preds` and `succs` must appear in that order, after `loop` if it is present. The printer omits `loop` when the flag is false and omits an empty id list. The parser accepts an explicit empty list (`preds:`).

Those lists are stored on the block. Lowering rebuilds predecessors and successors from `br` and `ret`, and ignores the stored lists if they disagree with the terminators.

`internal function @helper` is not a function body. It adds the name to the module's internal-function list. Lowering renames that symbol the same way it renames `internal` and `private` globals, so two objects can both contain a static function called `helper`.

## Instructions

Result instructions start with `%id =`. `store`, `ret`, `br`, `debugloc`, `asm`, and `comment` reject a result. `call` and `intrinsic` allow either form. A missing result on `add`, `load`, `phi`, and the other result opcodes is an error.

### Binary

```text
%6 = add i16 %4, %5
%4 = ne i16 %3, 0
```

`%id = op type lhs, rhs`

The type is the result type. It is not repeated on the operands.

| Opcode | Group |
|--------|--------|
| `add` `sub` `mul` | Arithmetic. |
| `sdiv` `udiv` `srem` `urem` | Signed and unsigned division and remainder. |
| `and` `or` `xor` | Bitwise. |
| `shl` `lshr` `ashr` | Shift left, logical shift right, arithmetic shift right. |
| `eq` `ne` | Equality. |
| `slt` `sle` `sgt` `sge` | Signed comparison. |
| `ult` `ule` `ugt` `uge` | Unsigned comparison. |

### Unary

```text
%1 = sext i32 %0
%2 = trunc i16 %0
%3 = ptrtoint i32 %0
```

`%id = op type operand`

| Opcode | Meaning |
|--------|---------|
| `not` | Bitwise complement. |
| `neg` | Arithmetic negation. |
| `zext` | Zero-extend. `type` is the destination width. |
| `sext` | Sign-extend. |
| `trunc` | Truncate. |
| `ptrtolinear` | Linear cell address, for pointer subtraction across banks. |
| `ptrtoint` | Pointer to integer. |
| `inttoptr` | Integer to pointer. |

There is no source type on the instruction. The operand is a value, and `type` is the result type.

### Memory

```text
%4 = load i16, fat(%2, stack)
%5 = load volatile i16, global @g
store i16 %0, fat(%2, stack)
store volatile i32 1, fat(%2, stack)
```

`load` is `%id = load [volatile] type, pointer`. The comma is required.

`store` is `store [volatile] type value, pointer`. The type is the type of the stored value, so a small constant still stores a wide `i32` or `i64`.

`volatile` is omitted when the access is not volatile.

### Alloca

```text
%2 = alloca i16 -> i16*
%3 = alloca i16, 4 -> i16*
%0 = alloca struct Point { i16, i16 } -> struct Point { i16, i16 }*
```

`%id = alloca alloc_type [, count] -> result_type`

`count` is a value. Omit it for one element. `result_type` is whatever the frontend stored, usually a pointer to `alloc_type`. The address temporary is paired with `fat(%id, stack)` at uses.

### GetElementPtr

```text
%1 = getelementptr i16*, i16 fat(%0, mixed), 1
%1 = getelementptr i16, struct Point { i16, i16 } fat(%0, stack), 1 field
%1 = getelementptr i16*, i16 fat(%0, stack)
```

`%id = getelementptr result_type, source_element_type pointer, index, index, ... [field]`

| Piece | Meaning |
|-------|---------|
| `result_type` | Type of the pointer this instruction produces. |
| `source_element_type` | What the pointer indexes into. |
| indexes | Values. There may be zero. |
| `field` | The indexes are word offsets into a struct. Without `field`, each index is scaled by the size of `source_element_type`. |

### Call and intrinsic

```text
%0 = call i16 global @"add"(1, 2)
call void func @abort()
%0 = call i16 global @printf(global @fmt, 1) stack 1
%1 = intrinsic i32 @"needs quotes"(undef)
intrinsic void @llvm.trap()
```

`[ %id = ] call result_type callee(arg, arg, ...) [stack N]`

`[ %id = ] intrinsic result_type @name(arg, arg, ...)`

Arguments are values. Their types are not repeated. `stack N` applies only to `call`. The intrinsic name is an `@` name, not a `func` or `global` value.

### Control

```text
ret %6
ret void
br label 3
br %4, label 1, label 2
%0 = phi i16 [1, label 1], [2, label 2]
%0 = phi i1
%1 = select i16 %c, 1, 0
%2 = cast %0 to i64
```

| Form | Meaning |
|------|---------|
| `ret value` / `ret void` | Return. |
| `br label N` | Unconditional branch to block N. |
| `br value, label T, label F` | Conditional branch. |
| `phi type [value, label N], ...` | One incoming pair per predecessor edge you want to keep. An empty `phi` has no pairs. |
| `select type cond, true_value, false_value` | Choose a value. All three operands share the written result type in the text; only one type is stored. |
| `cast value to type` | Convert `value` to `type`. |

### Debug, comments, and assembly

```text
debugloc "dir/a.c" 10 2
debugloc demo.c 4 1
comment "hello\n\"world\""
comment ""
asm "nop"
asm "LI %0, 42" outs "=r"(fat(%0, stack))
asm "ADD %0, %1, %2" outs "=r"(%0) tie 0 ins "r"(%1), "r"(%2) clobbers "t0", "memory"
```

`debugloc` stores a source location on that instruction: file, line, column. `comment` stores a string and is lowered to an assembly comment. Both are real instructions, so they occupy their own line and round-trip. `;` comments do not.

`asm` stores the assembly text plus output operands, input operands, and clobber strings. `tie N` sets `tied_to` to `N`. Omitting `tie` stores "not tied", which is different from `tie 0`.

## What lowering checks

Parsing accepts any module the grammar describes. Lowering then checks the module it is going to compile.

- The first block in the function is the entry. Later blocks are reached only by `br`, or by fall-through.
- A block whose last instruction is not `ret` or `br` falls through to the next block in the function.
- A `ret` or `br` that is not the last instruction in its block is an error.
- A branch to a block id that does not exist in that function is an error. Duplicate block ids are an error.
- `preds`, `succs`, and `type` entries do not override those rules.
- `function external` is skipped. Its body is not compiled.
- Linkage `declaration` does not reserve global storage.
- `internal function` names, and globals with linkage `internal` or `private`, are renamed with a per-object prefix so they stay local to the translation unit.
- A pointer use needs a bank lowering can materialize: `global`, `stack`, `heap(n)`, or `mixed` on a temporary that already has a recorded bank. `unknown` and `null` parse, and dereferencing them aborts. An indirect call whose target has no bank is an error.
- `field` versus a scaled GEP changes the address calculation. The text stores the choice; it does not infer it from the type.

`rcc compile file.ir` stops on the first parse error or the first lowering error and prints that error.
