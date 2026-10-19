# RCC bit-field layout

RCC supports integer bit-fields on both native and Ripple targets with the
same C16 layout. This was needed by the guest wolfSSL port; the ordinary
`char`, integer and pointer ABI remains unchanged.

## Storage rules

- Fields use the declared integer type's storage size. A C16 `unsigned int`
  unit is one 16-bit cell; `unsigned long` uses two cells and `unsigned long
  long` uses four. `_Bool` permits width one.
- Consecutive fields with equal storage sizes share a unit while they fit.
  The first occupies its least significant bits. Fields never straddle units.
- An ordinary member, a change of unit size, or an unnamed zero-width field
  ends the current unit. The existing aggregate ABI has no extra alignment
  padding. A union's fields all start at bit zero of cell zero.
- Unnamed nonzero fields reserve bits and are skipped by positional
  initializers. A zero-width field must be unnamed and has no storage of its
  own.
- Widths must be nonnegative integer constant expressions no larger than the
  base type's bit width. Named zero-width fields and noninteger bases are
  rejected. Address-of and `sizeof` cannot be applied to a bit-field.

For example, this structure occupies two cells:

```c
struct Flags {
    unsigned ready : 1;  /* cell 0, bit 0 */
    unsigned mode  : 3;  /* cell 0, bits 1..3 */
    unsigned       : 0;
    unsigned count : 8;  /* cell 1, bits 0..7 */
};
```

Signed fields are sign-extended when read. Assignment truncates to the field
width; `_Bool` normalizes to zero or one. Narrow fields use integer promotion
in arithmetic. Read-modify-write operations preserve neighboring fields and
evaluate the destination address once. Volatile updates read and write the
containing storage unit once; this is not an atomic-operation guarantee.

Static and automatic initialization, union aliases, aggregate copies and
increment/compound assignment use the same packing. Do not use this layout
as a network wire format: protocols still require explicit octet encoding.

## Validation

The frontend regression cases are `rcc-frontend/tests/bit_fields.rs` and the
layout suite. `c-test/tests/types/test_bit_fields.c` exercises packed aliases,
signed values, adjacent-field preservation, initializer packing, zero-width
boundaries, volatile access, Boolean behavior, integer promotion and 32/64-bit
storage units. It passes on native and RVM JIT.

Related compiler fixes needed by wolfSSL include unsigned 64-bit preprocessing
arithmetic, ignored macro definitions in inactive conditional branches,
grouped declarators, typedef-array parameter adjustment and constant object
`sizeof` bounds. These are C features; GNU statement expressions and variadic
macro extensions were not added by this port.
