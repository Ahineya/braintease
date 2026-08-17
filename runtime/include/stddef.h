#ifndef STDDEF_H
#define STDDEF_H

/* Standard definitions */

#ifndef NULL
#define NULL ((void *)0)
#endif

/* Unsigned 16-bit; matches sizeof and BANK_SIZE 64000. */
typedef unsigned int size_t;

typedef int ptrdiff_t;

#define offsetof(type, member) ((size_t)&((type *)0)->member)

#endif /* STDDEF_H */
