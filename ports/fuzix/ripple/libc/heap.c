/* SPDX-License-Identifier: GPL-2.0-or-later */
#if RIPPLE_MMU
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct block { size_t size; unsigned long next; unsigned available; };
static unsigned long first, last;
#define HEADER ((size_t)sizeof(struct block))
static struct block *at(unsigned long p) { return (struct block *)p; }
static unsigned long after(unsigned long p, size_t n) {
    return (unsigned long)((char *)p+n);
}
static unsigned long find(void *p) {
    unsigned long q=first;
    while (q) {
        if (after(q,HEADER)==(unsigned long)p) return q;
        q=at(q)->next;
    }
    return 0;
}
void *malloc(size_t n) {
    unsigned long q=first, tail, current;
    struct block *b, *split;
    if (!n) n=1;
    if (n>16UL*64000UL-HEADER) {errno=ENOMEM;return NULL;}
    while (q) {
        b=at(q);
        if (b->available && b->size>=n) {
            if (b->size-n>HEADER) {
                tail=after(q,HEADER+n); split=at(tail);
                split->size=b->size-n-HEADER; split->next=b->next; split->available=1;
                b->next=tail; b->size=n;
                if (last==q) last=tail;
            }
            b->available=0;
            return (char *)b+HEADER;
        }
        q=b->next;
    }
    current=(unsigned long)sbrk(0);
    if (current==0xffffffffUL || brk((void *)after(current,n+HEADER))<0) return NULL;
    b=at(current); b->size=n; b->next=0; b->available=0;
    if (last) at(last)->next=current; else first=current;
    last=current;
    return (char *)b+HEADER;
}
void free(void *p) {
    unsigned long q;
    struct block *b, *next;
    if (!p) return;
    q=find(p);
    if (!q) {errno=EINVAL;return;}
    at(q)->available=1;
    q=first;
    while (q) {
        b=at(q);
        if (b->next) {
            next=at(b->next);
            if (b->available && next->available && after(q,HEADER+b->size)==b->next) {
                if (last==b->next) last=q;
                b->size+=HEADER+next->size; b->next=next->next;
                continue;
            }
        }
        q=b->next;
    }
}
void *calloc(size_t count,size_t size) {
    size_t n;
    void *p;
    if (size && count>~0UL/size) {errno=ENOMEM;return NULL;}
    n=count*size; p=malloc(n);
    if (p) memset(p,0,n);
    return p;
}
void *realloc(void *p,size_t n) {
    unsigned long q;
    void *r;
    if (!p) return malloc(n);
    if (!n) {free(p);return NULL;}
    q=find(p);
    if (!q) {errno=EINVAL;return NULL;}
    if (n<=at(q)->size) return p;
    r=malloc(n);
    if (r) {memcpy(r,p,at(q)->size);free(p);}
    return r;
}
#else
/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Process-local allocator. Free-list links are offsets in the user GP bank;
 * no ordering of pointers to unrelated allocations or relative bank tags. */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
struct block {
    unsigned size, next, available;
};
static unsigned first, last;
#define HEADER ((unsigned)sizeof(struct block))
static struct block *at(unsigned off) {
    return (struct block *)(0x80000UL + off);
}
static unsigned find(void *p) {
    unsigned long v = (unsigned long)p;
    unsigned off = first;
    if ((v >> 16) != 8 || (unsigned)v < HEADER)
        return 0;
    while (off) {
        if (off + HEADER == (unsigned)v)
            return off;
        off = at(off)->next;
    }
    return 0;
}
void *malloc(size_t n) {

    unsigned off = first, want;
    struct block *b, *split;
    unsigned long current, grow;
    if (!n)
        n = 1;
    if (n > 62976UL - HEADER) {
        errno = ENOMEM;
        return NULL;
    }
    want = (unsigned)n;
    while (off) {
        b = at(off);
        if (b->available && b->size >= want) {
            if (b->size - want > HEADER) {
                unsigned tail = off + HEADER + want;
                split = at(tail);
                split->size = b->size - want - HEADER;
                split->next = b->next;
                split->available = 1;
                b->next = tail;
                b->size = want;
                if (last == off)
                    last = tail;
            }
            b->available = 0;
            return b + 1;
        }
        off = b->next;
    }

    current = (unsigned long)sbrk(0);
    if (current == 0xffffffffUL || (current >> 16) != 8) {
        errno = ENOMEM;
        return NULL;
    }
    grow = n + HEADER;
    if ((current & 65535UL) + grow >= 62976UL) {
        errno = ENOMEM;
        return NULL;
    }
    if (brk((void *)(current + grow)) < 0)
        return NULL;

    off = (unsigned)current;
    b = at(off);
    b->size = want;
    b->next = 0;
    b->available = 0;
    if (last)
        at(last)->next = off;
    else
        first = off;
    last = off;

    return b + 1;
}
void free(void *p) {
    unsigned off;
    struct block *b, *next;
    if (!p)
        return;
    off = find(p);
    if (!off) {
        errno = EINVAL;
        return;
    }
    at(off)->available = 1;
    off = first;
    while (off) {
        b = at(off);
        if (b->next) {
            next = at(b->next);
            if (b->available && next->available &&
                off + HEADER + b->size == b->next) {
                if (last == b->next)
                    last = off;
                b->size += HEADER + next->size;
                b->next = next->next;
                continue;
            }
        }
        off = b->next;
    }
}
void *calloc(size_t count, size_t size) {
    void *p;
    size_t n;
    if (size && count > ~0UL / size) {
        errno = ENOMEM;
        return NULL;
    }
    n = count * size;
    p = malloc(n);
    if (p)
        memset(p, 0, n);
    return p;
}
void *realloc(void *p, size_t n) {
    unsigned off;
    void *q;
    if (!p)
        return malloc(n);
    if (!n) {
        free(p);
        return NULL;
    }
    off = find(p);
    if (!off) {
        errno = EINVAL;
        return NULL;
    }
    if (n <= at(off)->size)
        return p;
    q = malloc(n);
    if (!q)
        return NULL;
    memcpy(q, p, at(off)->size);
    free(p);
    return q;
}

#endif
