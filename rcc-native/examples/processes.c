/* First hosted-Riplix milestone: fork real native C continuations, including
 * nested calls, live values, pointers into outer frames, and private heaps. */
#include <native_process.h>
#include <stdlib.h>

extern int puts(const char *);
static int global_value = 7;

static int nested(int depth, int *outer, int *heap)
{
    volatile int local = 10 + depth;
    unsigned long long wide = 0xfedcba9876543210ULL + depth;
    int pid;
    if (depth)
        pid = nested(depth - 1, outer, heap);
    else {
        pid = rcc_process_fork();
        if (pid < 0)
            rcc_process_exit(90);
        rcc_process_yield();
        if (pid == 0) {
            global_value = 99;
            *outer = 33;
            *heap = 44;
        }
        rcc_process_yield();
        if (global_value != (pid ? 7 : 99) ||
            *outer != (pid ? 3 : 33) || *heap != (pid ? 4 : 44))
            rcc_process_exit(91);
    }
    if (local != 10 + depth || wide != 0xfedcba9876543210ULL + depth)
        rcc_process_exit(92);
    return pid + local;
}

int main(void)
{
    int outer = 3;
    int status = -1;
    int *heap = malloc(sizeof(int));
    if (!heap)
        return 1;
    *heap = 4;
    puts("native: before fork");
    int result = nested(2, &outer, heap);
    if (rcc_process_getpid() != 1) {
        if (result != 33 || outer != 33 || *heap != 44)
            return 2;
        free(heap);
        puts("native: child resumed through three C frames");
        return 23;
    }
    int child = result - 33;
    if (child <= 1 || rcc_process_wait(child, &status) != child || status != 23)
        return 3;
    if (outer != 3 || *heap != 4 || global_value != 7)
        return 4;
    free(heap);
    if (rcc_process_wait(child, &status) != -1)
        return 5;
    puts("native: parent reaped child; stack, globals and heap are independent");
    return 0;
}
