#ifndef STDDEF_H
#define STDDEF_H

/* Standard definitions */

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Object sizes and pointer differences may span multiple 16-bit banks.
 * Units are C bytes, each one 16-bit addressable VM cell. */
typedef unsigned long size_t;

typedef long ptrdiff_t;

#define offsetof(type, member) ((size_t)&((type *)0)->member)

#endif /* STDDEF_H */
