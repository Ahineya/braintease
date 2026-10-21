/* RCCMOD01 is compiler/linker metadata carried as ordinary native ELF data.
 * All lengths/offsets count octets. Integers are explicit little endian.
 * No host pointer, C struct layout, or guest C16 layout enters this wire. */
#ifndef RCC_PORTABLE_NATIVE_FORMAT_H
#define RCC_PORTABLE_NATIVE_FORMAT_H
#define NM_SYMBOL "rcc_module_metadata"
#define NM_MAGIC "RCCMOD01"
#define NM_HEADER 32UL
#define NM_RECORD 64UL
#define NM_FUNCTION 1
#define NM_GLOBAL 2
#define NM_INITIALIZER 3
#define NM_CELLS 4
#define NM_DEFINED 1
#define NM_INTERNAL 2
#define NM_ENTRY 4
/* Header: magic[8], total:u32, count:u32, entry_record_plus_one:u32,
 *         reserved[3]:u32.
 * Record: kind:u32, flags:u32, size:u32, arity_or_cells:u32,
 *         slot:u64, offset:u64, value:u64, width:u32, mode:u32,
 *         name_length:u32, signature_length:u32, reserved:u64, name[], signature[].
 * Strings include NUL; records are padded with zeros to eight octets.
 * Function/global slots are zero in .o, patched by the final linker.
 * Initializer modes: 0=bits, 1=logical data address, 2=function token.
 * Its signature field instead names the referenced symbol (empty for bits).
 * Pointer initializers carry a signed cell addend in value. */
/* NM_CELLS: cells is the number of contiguous C16 cells at offset; flags,
 * slot, value, width and mode are zero, signature is empty. Immediately after
 * the two strings follow cells*2 little-endian octets, then zero padding.
 * Older scalar/symbolic initializer records remain accepted. */
/* Signatures retain scalar IR kinds/widths. A final ... denotes a variadic
 * function and counts as one hidden logical cursor in arity_or_cells.
 * It is distinct from an explicit p32 parameter; tails use packed C16 cells.
 * Aggregate globals use m<cells>: RCC IR flattens external declarations into
 * cell arrays and definitions into initializer-storage chunks. m0 is an
 * incomplete external array, completed by its defining unit. Consequently
 * aggregate extents are checked, not erased source C field types/qualifiers. */
#endif
