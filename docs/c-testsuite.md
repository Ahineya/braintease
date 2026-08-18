# c-testsuite vs Ripple C

Vendored [c-testsuite](https://github.com/c-testsuite/c-testsuite) single-exec
tests, run with `c-test/vendor/c-testsuite/runners/single-exec/rcc`.

Snapshot from `c-test/vendor/c-testsuite/moo.txt` (162 pass / 55 fail / 3 skip
of 220). Categories below are for that run. Tests that cannot pass on this
target are listed in `runners/single-exec/rcc.skip` and should not be treated
as compiler regressions.

Ripple C data model (ILP16-ish):

| Type | `sizeof` (bytes) |
|------|------------------|
| `char` | 1 |
| `short` / `int` | 2 |
| `long` / `float` / pointer (fat) | 4 |
| `long long` / `double` | 8 |

`int` is 16-bit. Host-oriented tests that hard-code `sizeof(int) == 4`, 32-bit
`%d` of large constants, or only `__ILP32__` / `__LP64__` / `__LLP64__` are
false negatives.

---

## Already skipped

| Test | Why |
|------|-----|
| `00174.c` | `#include <math.h>` — runtime has no math library |
| `00220.c` | `#include <wchar.h>` — no wide-character library |
| `00204.c` | `long double` is not a distinct type; also an AArch64 ABI stress test |

---

## Architecture / ABI false negatives

These compile (or would, aside from an unrelated bug) but their **golden
output assumes a mainstream 32/64-bit C ABI**. Skip them; do not chase the
expected file.

| Test | Why it cannot match |
|------|---------------------|
| `00178.c` | Prints `sizeof(char/int/double/!x)` expecting `1, 4, 8, 4`. Ripple prints `1, 2, 8, 2`. |
| `00168.c` | `factorial(10)` in `int` — 3 628 800 does not fit in a 16-bit `int` (overflow starts at `8!`). |
| `00166.c` | Expected `printf("%d")` of `0x2468ac` as 2 386 092. That value does not fit in 16-bit `int`/`%d`. (The compiler also currently truncates the literal instead of typing it as `long`; even a correct type would not match this expected file.) |
| `00212.c` | Only recognizes `__ILP32__` / `__LP64__` / `__LLP64__`. Ripple is none of those (16-bit `int`, 32-bit `long`, 32-bit fat pointer). |

`00200.c` uses `sizeof` in a portable way (`PTYPE`); it fails because the
`PTYPE` macro is not expanded (preprocessor), not because of the data model.

---

## Preprocessor

| Test | Failure | Notes |
|------|---------|-------|
| `00141.c` | `Undefined variable: CAT` | `##` token paste (`CAT(foo,bar)` → `foobar`) |
| `00145.c` | `Division by zero in #if expression` | `#if` must short-circuit `0 && (0/0)` and `1 \|\| (0/0)` |
| `00152.c` | `Expected line number in line directive` | `#define line 1000` then `#line line` |
| `00200.c` | `Undefined variable: PTYPE` | nested function-like macros (`PTYPE` / `CHECK` / `TEST4`) |
| `00201.c` | `Undefined variable: AB` | `CAT(A,B)(x)` → `AB(x)` → `xy` |
| `00202.c` | `Macro 'Q' expects 2 arguments, got 1` | empty macro arguments (`Q(+,)` ) |
| `00206.c` | parse: `Expected ) in function call, found 111` | `#pragma push_macro` / `pop_macro` (GCC/TCC extension) |
| `00211.c` | output `0xe+1 = 15` vs `n+1 = 15` | `#define n 0xe` leaked into the format string |

---

## Parser

C99 (or common extension) syntax the frontend does not accept.

| Test | Failure | Feature |
|------|---------|---------|
| `00098.c` | `Expected ; in return statement, found '\0'` | wide char `L'\0'` |
| `00159.c` | `Expected primary expression, found void` | call through a cast to function pointer: `((void(*)(void))0)()` |
| `00162.c` | `Expected primary expression, found const` | C99 `int x[const 5]`, `[static 5]`, `[restrict 5]` in parameters |
| `00189.c` | `Expected type specifier` | `FILE *` in a declarator (also missing `FILE` in libc) |
| `00207.c` | `Array size must be a constant expression` | VLA `char test[argc]` and `1 && 1` as a size |
| `00209.c` | `Expected type specifier` | `int f1(int (), int)` and `int ([4])` — abstract prototype parameters |
| `00210.c` | `Expected type specifier` | `__attribute__((packed))` / `stdcall` / `noinline` |
| `00213.c` | `Expected primary expression, found {` | GNU statement expression `({ ... })` |
| `00214.c` | same | GNU statement expressions + `__builtin_expect` |
| `00216.c` | `Expected ] in array designator, found ...` | range designator `[1 ... 5]` (GCC) |
| `00218.c` | `Expected ; in struct field, found :` | bit-fields |
| `00219.c` | `Expected primary expression, found int` | C11 `_Generic` |

---

## Semantic analysis

Parses, then type-checks or name resolution fails.

| Test | Failure | Notes |
|------|---------|-------|
| `00120.c` | `Undefined variable: X` | enum constant declared inside an anonymous struct |
| `00124.c` | `Undefined variable: a` | returning / using a function pointer (`f1` returns `f2`) |
| `00144.c` | `Type mismatch: expected void*, found int` | pointer / integer / `void*` ternary (`i ? 0 : (void*)0`) |
| `00171.c` | `Undefined variable: NULL` | `NULL` not provided by `stdio.h` (header gap; listed here because it surfaces as a semantic error) |
| `00179.c` | `Type mismatch: expected void*, found char[10]` | `memset`/`memcpy` vs array decay to `void*` |
| `00205.c` | `Type mismatch: expected struct, found long` | unbraced / messy struct-array initializer (J snippet) |
| `00208.c` | `Type mismatch: expected char[9], found char[]` | `char s[9] = "nonono"` (string init of a fixed array) |

---

## Codegen / backend

Frontend accepts the program; IR or lowering does not.

| Test | Failure | Notes |
|------|---------|-------|
| `00049.c` | `non-constant array initializer` | designated init of a global struct with `&x` |
| `00072.c` | `Pointer must be a fat pointer ... Temp` | `p += 1` on `int *` |
| `00073.c` | same | `p -= 1` |
| `00078.c` | `Invalid function value for call: FatPtr` | block-scope `int f1(char *);` treated as a pointer object (see `test_local_function_prototype`) |
| `00087.c` | `Function pointers not yet implemented` | `v.fptr = foo; v.fptr()` |
| `00089.c` | `non-constant array initializer` | global init `&zero` as a function pointer |
| `00095.c` | `Unexpected value type in FatPtr address: Global("main")` | `return &main` |
| `00112.c` | `Unexpected Value::Global('__str_0')` | `"abc" == (void*)0` |
| `00123.c` | `No current function` | file-scope `double x = 100.0` |
| `00129.c` | `Struct pointer must be a fat pointer` | nested struct tags + `goto` (stress test; lowering) |
| `00149.c` | compound literal: `No current function` | file-scope `&(struct S){1,2}` |
| `00150.c` | same | nested file-scope compound literal |
| `00170.c` | `Unknown global variable: it_real_fn` | function pointer field `s->f2 = it_real_fn` |
| `00182.c` | `Pointer must be a fat pointer` in `print_led` | pointer walking a buffer |

Runtime wrong answers that are still compiler bugs (the binary ran):

| Test | What happened | Notes |
|------|----------------|-------|
| `00077.c` | nonzero exit | `sizeof` of an `int x[100]` parameter vs `sizeof(void*)` — parameter should decay |
| `00092.c` | nonzero exit | `int a[] = {5, [2]=2, 3}` — `sizeof(a)` should be `4 * sizeof(int)` |
| `00093.c` | nonzero exit | `int a[] = {1,2,3,4}` — `sizeof(a) != 4*sizeof(int)` (portable check; not a 4-byte-`int` assumption) |
| `00119.c` | nonzero exit | `double x = 100; return x < 1` should be 0 |
| `00173.c` | truncated string walk | `for (b = a; *b; b++)` stops after the first character |
| `00197.c` | statics wrong | file-scope `static int fred = 1234` prints 0; function-scope static does not increment |

---

## Runtime / libc

Missing headers, missing functions, or `printf` conversions. Not frontend
syntax.

| Test | Failure | Notes |
|------|---------|-------|
| `00175.c` | output still has `%f` | `printf("%f")` not implemented (char/int conversions look fine) |
| `00186.c` | `Undefined variable: sprintf` | no `sprintf` |
| `00187.c` | `Undefined variable: FILE` | no `FILE` / `fopen` / `fread` |
| `00189.c` | (also parser) | function-pointer init of `fprintf`; needs `FILE` |
| `00195.c` | output `%f, %f` | `printf("%f")` of `double` struct fields |

`00174.c` (math) and `00220.c` (wchar) belong here too; they are already skipped.

---

## Suggested skip list (ABI only)

Keep `rcc.skip` to tests that **must not** pass with the current data model
or missing system headers, not to “hard” compiler bugs:

```
00174.c   math.h
00220.c   wchar.h
00204.c   long double
00178.c   sizeof(int)==4
00168.c   16-bit int overflow (10!)
00166.c   32-bit int constants via %d
00212.c   no matching __*LP*__ data-model macro
```

Everything else in this file is a real missing feature or a bug.

---

## rct mirrors

Local tests that track the same holes (so they do not disappear when the
vendor suite is not run):

| rct test | Vendor | Status |
|----------|--------|--------|
| `test_unnamed_pointer_param` | `00025.c` | passing |
| `test_anonymous_members` | `00046.c` / `00050.c` | passing |
| `test_struct_forward_decl` | `00044.c` | passing |
| `test_local_struct_tag` / `test_local_struct_tag_only` | `00018.c` / `00052.c` | passing |
| `test_local_function_prototype` | `00078.c` | **failing** (block-scope prototype → fat pointer) |
