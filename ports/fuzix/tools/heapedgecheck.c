/* Allocator integration cases outside the measured scaling workload. */
#define main scaling_main
#include "heapcheck.c"
#undef main
#include <sys/wait.h>

static int near_limit(void) {
    char *p;
    /* First allocation in this child: the usual 4096-cell growth cannot fit,
     * but one small block still can. Parent's heap remains untouched. */
#ifdef FUSE_X_USER
    if((unsigned long)sbrk(0)<(48UL<<16))C(brk((void *)(48UL<<16))==0);
    C(brk((void *)((239UL << 16) | 63980U)) == 0);
#else
    C(brk((void *)((30UL << 16) | 63980U)) == 0);
#endif
    p = malloc(1); C(p != NULL); *p = 0xabcd;
    C(realloc(p, 10000) == NULL && (unsigned char)*p == 0xabcd);
#ifdef FUSE_X_USER
    C(malloc(2500000UL) == NULL && (unsigned char)*p == 0xabcd);
#endif
    free(p); p = malloc(1); C(p != NULL); free(p);
    return 0;
}
static int isolated_heap(char *p) {
    char *q;
    C((unsigned char)*p == 0xbeef);
    free(p); q = malloc(20000); C(q != NULL); memset(q, 0x1234, 20000); free(q);
    return 0;
}
int main(void) {
    pid_t pid; int status;
    char *p, *q, *guard, *gap;
    unsigned long padding;
    pid = fork(); C(pid >= 0);
    if (!pid) _exit(near_limit());
    C(waitpid(pid, &status, 0) == pid && status == 0);

    /* Make the next allocator header begin at a bank's very last cell.
     * The direct sbrk reservation must never become part of a free block. */
    p = malloc(1); C(p != NULL); *p = 0x4567;
    gap = sbrk(0); padding = 63999U - (unsigned)(unsigned long)gap;
    if (padding) { C(sbrk(padding) == gap); gap[0] = 0xcafe; gap[padding-1] = 0xfeed; }
    q = malloc(70003UL); C(q != NULL);
    q[0] = 0xabcd; q[64000UL] = 0xfeed; q[70002UL] = 0x1234;
    C((unsigned char)*p == 0x4567);
    free(p); free(q);
    q = malloc(90000UL); C(q != NULL); memset(q, 0x9876, 90000UL); free(q);
    if (padding) C((unsigned char)gap[0] == 0xcafe && (unsigned char)gap[padding-1] == 0xfeed);

    /* Isolate neighbors with a live guard, then grow and shrink in place. */
    p = malloc(80); q = malloc(90); guard = malloc(100);
    C(p && q && guard); memset(p, 0xabcd, 80); *guard = 0xfeed;
    free(q); q = realloc(p, 150); C(q == p && (unsigned char)q[79] == 0xabcd);
    p = realloc(q, 12); C(p == q && (unsigned char)*p == 0xabcd);
    q = malloc(80); C(q != NULL && (unsigned char)*guard == 0xfeed);
    free(p); free(q); free(guard);

    p = malloc(20000); C(p != NULL); *p = 0xbeef;
    pid = fork(); C(pid >= 0);
    if (!pid) _exit(isolated_heap(p));
    C(waitpid(pid, &status, 0) == pid && status == 0);
    C((unsigned char)*p == 0xbeef); free(p);
    C(scaling_main() == 0);
    puts("HEAP_EDGES_OK"); return 0;
}
