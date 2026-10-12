// Verify sizeof implementation
void putchar(int c);

int main() {
    // Test 1: char size
    putchar('1');
    putchar(':');
    if (sizeof(char) == 1) 
        putchar('Y');
    else 
        putchar('N');
    putchar(10);
    
    // Test 2: int size  
    putchar('2');
    putchar(':');
    if (sizeof(int) == 1)
        putchar('Y');
    else
        putchar('N');
    putchar(10);
    
    // Test 3: array size
    int arr[5];
    putchar('3');
    putchar(':');
    if (sizeof(arr) == 5)
        putchar('Y');
    else
        putchar('N');
    putchar(10);
    
    return 0;
}