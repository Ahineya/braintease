// Comprehensive sizeof test
void putchar(int c);

int main() {
    // Basic types
    if (sizeof(char) == 1) putchar('Y'); else putchar('N');
    if (sizeof(int) == 1) putchar('Y'); else putchar('N');
    if (sizeof(long) == 2) putchar('Y'); else putchar('N');
    if (sizeof(void*) == 2) putchar('Y'); else putchar('N');
    
    // Arrays
    char str[10];
    int nums[5];
    long longs[3];
    
    if (sizeof(str) == 10) putchar('Y'); else putchar('N');
    if (sizeof(nums) == 5) putchar('Y'); else putchar('N');
    if (sizeof(longs) == 6) putchar('Y'); else putchar('N');
    
    // Expressions (sizeof doesn't evaluate them)
    int x = 0;
    if (sizeof(x++) == 1) putchar('Y'); else putchar('N');
    if (x == 0) putchar('Y'); else putchar('N');  // x should still be 0
    
    putchar(10);
    return 0;
}