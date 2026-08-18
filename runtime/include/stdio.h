#ifndef STDIO_H
#define STDIO_H

#define EOF (-1)

void putchar(int c);
int puts(const char *s);
int getchar(void);
int printf(char *fmt, ...);
int scanf(char *fmt, ...);
int sscanf(char *s, char *fmt, ...);

#endif /* STDIO_H */
