#ifndef STDIO_H
#define STDIO_H

// Standard I/O functions

// Output a single character
void putchar(int c);

// Output a string followed by newline
// Returns 0 on success, -1 on error
int puts(const char *s);

// Read a single character from input
// Blocks until a character is available
int getchar(void);

int printf(char *fmt, ...);
// char *gets(char *s);

#endif // STDIO_H