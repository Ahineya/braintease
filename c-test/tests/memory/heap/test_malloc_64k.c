// Test malloc with 64000 byte allocation
#include <stdio.h>
#include <stdlib.h>

int main() {
    // Try to allocate exactly 63000 words (should succeed with 64000 BANK_SIZE)
    unsigned int* big_array = (unsigned int*)malloc(63000U);
    
    if (big_array == NULL) {
        putchar('N');  // Failed
        putchar('\n');
        return 1;
    }
    
    putchar('Y');  // Success - allocation worked
    putchar('\n');
    
    // Test that we can write to the array
    big_array[0] = 42;
    big_array[31499] = 99;  // Half of 63000
    
    if (big_array[0] == 42 && big_array[31499] == 99) {
        putchar('Y');  // Success - can write and read
    } else {
        putchar('N');
    }
    putchar('\n');
    
    free(big_array);
    return 0;
}