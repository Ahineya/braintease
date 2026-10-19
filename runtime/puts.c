// Runtime implementation of puts
// Outputs a string followed by newline
// The compiler automatically handles fat pointers

extern void putchar(int c);
extern void tty_write_bytes(const char *, unsigned short);

int puts(char *s) {
    // The compiler will pass s as a fat pointer (address + bank tag)
    // and handle the bank tag automatically for all operations
    
//     if (!s) return -1;
    
    // Output each character
    while (*s) {
        unsigned short n = 0;
        if (*s == '\n') { putchar('\n'); ++s; continue; }
        while (n < 4096 && s[n] && s[n] != '\n') ++n;
        tty_write_bytes(s, n);
        s += n;
    }
    
    // Output newline
    putchar('\n');
    
    return 0;  // Success
}
