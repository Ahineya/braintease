/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_STDIO_H
#define RIPPLE_STDIO_H
#include <stdarg.h>
#include <sys/types.h>
#define EOF (-1)
#define BUFSIZ 256
#define PATHLEN 512
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2
/* Buffers hold C characters, which are 16-bit on Ripple. */
typedef struct ripple_file {
    int fd, used, flags, eof, err, pushed, buffering;
    size_t pos, len;
    char buf[BUFSIZ];
} FILE;
typedef off_t fpos_t;
extern FILE *stdin, *stdout, *stderr;
int fgetc(FILE *);
int fputc(int, FILE *);
int ungetc(int, FILE *);
#define getc(f) fgetc(f)
#define putc(c, f) fputc(c, f)
#define getchar() fgetc(stdin)
#define putchar(c) fputc(c, stdout)
#define fileno(f) ((f)->fd)
#define feof(f) ((f)->eof)
#define ferror(f) ((f)->err)
#define clearerr(f) ((void)((f)->eof = (f)->err = 0))
FILE *fopen(const char *, const char *);
FILE *fdopen(int, const char *);
FILE *freopen(const char *, const char *, FILE *);
int fclose(FILE *);
int fflush(FILE *);
char *fgets(char *, size_t, FILE *);
int fputs(const char *, FILE *);
int puts(const char *);
size_t fread(void *, size_t, size_t, FILE *);
size_t fwrite(const void *, size_t, size_t, FILE *);
int fseek(FILE *, long, int);
long ftell(FILE *);
#define fseeko fseek
#define ftello ftell
void rewind(FILE *);
int fgetpos(FILE *, fpos_t *);
int fsetpos(FILE *, const fpos_t *);
void setbuf(FILE *, char *);
int setvbuf(FILE *, char *, int, size_t);
int printf(const char *, ...);
int fprintf(FILE *, const char *, ...);
int sprintf(char *, const char *, ...);
int snprintf(char *, size_t, const char *, ...);
int vprintf(const char *, va_list);
int vfprintf(FILE *, const char *, va_list);
int vsprintf(char *, const char *, va_list);
int vsnprintf(char *, size_t, const char *, va_list);
void perror(const char *);
int rename(const char *, const char *);
int remove(const char *);
/* Historical applications rely on declarations traditionally pulled in here. */
#include <unistd.h>
#endif
