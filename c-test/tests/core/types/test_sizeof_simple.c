// Simple sizeof test
void putchar(int c);

int main() {
    // Test sizeof on types
    int size_char = sizeof(char);
    int size_int = sizeof(int);
    int size_ptr = sizeof(int*);
    
    // Test sizeof on array
    int arr[3];
    int size_arr = sizeof(arr);
    
    // Print sizes as digits (for simple testing)
    putchar('0' + size_char);  // Should be 1
    putchar(' ');
    putchar('0' + size_int);   // Should be 1
    putchar(' ');
    putchar('0' + size_ptr);   // Should be 2
    putchar(' ');
    putchar('0' + size_arr);   // Should be 3 (3 * 1)
    putchar('\n');
    
    return 0;
}