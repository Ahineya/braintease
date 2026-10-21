/* The same native ABI fixture can run hosted or on the freestanding kernel. */
#include <stdio.h>
#include <stdlib.h>
#include <native_hosted.h>
#include <native_process.h>
static int private_value = 7;
static char private_banks[70000UL];
static int twice(int x) { return x * 2; }
static int (*callback)(int) = twice;
static void require(int condition, const char *message) {
    if (!condition) { puts(message); exit(1); }
}
static void fresh(unsigned long argument) {
    unsigned *shared = (void *)argument;
    require(private_value == 7 && callback(9) == 18, "exec initializers failed");
    require(private_banks[0] == 0 && private_banks[65000UL] == 0 && private_banks[69999UL] == 0,
            "exec did not reset extended global banks");
    require(*shared == 123, "exec lost shared kernel data");
    rcc_process_kernel_free(shared);
    puts("RCC_FREESTANDING_PROCESS_CHECKS_OK");
}
int main(void) {
    unsigned *shared = rcc_process_kernel_alloc(1);
    char *heap = malloc(80000UL);
    int local = 11, round;
    require(shared && heap, "allocation failed");
    heap[0] = 'a'; heap[65000UL] = 'b'; heap[79999UL] = 'c';
    private_banks[0] = 'd'; private_banks[65000UL] = 'e'; private_banks[69999UL] = 'f';
    require(rcc_hosted_valid_function((unsigned long)fresh, 1) &&
            !rcc_hosted_valid_function((unsigned long)fresh, 0) &&
            !rcc_hosted_valid_function(0, 1), "function validation failed");
    require(rcc_hosted_clone(0) == -1 && rcc_hosted_clone(64) == -1, "invalid clone slot accepted");
    for (round = 0; round < 4; ++round) {
        int child = rcc_hosted_clone(63);
        require(child >= 0, "high-slot clone failed");
        if (!child) {
            private_value = 99; local = 22; heap[65000UL] = 'x'; *shared = round + 11;
            private_banks[65000UL] = 'y';
            rcc_hosted_switch(0);
            require(private_value == 99 && local == 22 && heap[65000UL] == 'x' && private_banks[65000UL] == 'y', "child state was not resumed");
            *shared = round + 21;
            rcc_hosted_drop_switch(0);
            require(0, "discarded continuation returned");
        }
        require(rcc_hosted_clone(63) == -1, "occupied clone slot accepted");
        rcc_hosted_switch(63);
        require(private_value == 7 && local == 11 && heap[65000UL] == 'b' && *shared == (unsigned)round + 11,
                "parent isolation/shared data failed");
        rcc_hosted_switch(63);
        require(private_value == 7 && heap[0] == 'a' && heap[79999UL] == 'c' && *shared == (unsigned)round + 21,
                "drop or slot reuse failed");
        require(private_banks[0] == 'd' && private_banks[65000UL] == 'e' && private_banks[69999UL] == 'f',
                "extended global banks lost isolation");
    }
    heap = realloc(heap, 90000UL);
    require(heap && heap[65000UL] == 'b' && heap[79999UL] == 'c', "heap state did not survive switches");
    free(heap); private_value = 31; *shared = 123;
    rcc_hosted_exec(fresh, (unsigned long)shared);
    require(0, "exec returned");
    return 1;
}
