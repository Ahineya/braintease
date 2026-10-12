// Test sizeof operator
void putchar(int c);

int main() {
    // Test sizeof on types
    int size_char = sizeof(char);
    int size_int = sizeof(int);
    int size_long = sizeof(long);
    int size_ptr = sizeof(int*);
    
    // Test sizeof on expressions
    int x = 42;
    int arr[10];
    int size_var = sizeof(x);
    int size_arr = sizeof(arr);

    // Verify sizes are correct
    if (size_char == 1) {
        putchar('1');
    } else {
        putchar('X');
    }
    
    if (size_int == 1) {
        putchar('2');
    } else {
        putchar('X');
    }
    
    if (size_long == 2) {
        putchar('3');
    } else {
        putchar('X');
    }
    
    if (size_ptr == 2) {
        putchar('4');
    } else {
        putchar('X');
    }
    
    if (size_var == 1) {
        putchar('5');
    } else {
        putchar('X');
    }
    
    if (size_arr == 10) {
        putchar('6');
    } else {
        putchar('X');
    }

    putchar('\n');
    
    return 0;
}