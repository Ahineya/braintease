#ifndef DOOM_PORT_STDIO_H
#define DOOM_PORT_STDIO_H
#include "../../../runtime/include/stdio.h"
#include <stddef.h>
#include <stdarg.h>

typedef struct doom_port_file FILE;
extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#define FILENAME_MAX 256
#define BUFSIZ 256

FILE *fopen(const char *path, const char *mode);
FILE *freopen(const char *path, const char *mode, FILE *stream);
int fclose(FILE *stream);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
int fseek(FILE *stream, long offset, int origin);
long ftell(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
int fflush(FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int fputc(int ch, FILE *stream);
int putc(int ch, FILE *stream);
int fputs(const char *text, FILE *stream);
int remove(const char *path);
int rename(const char *old_path, const char *new_path);
int fprintf(FILE *stream, const char *format, ...);
int vfprintf(FILE *stream, const char *format, va_list args);
int sprintf(char *buffer, const char *format, ...);
int vsprintf(char *buffer, const char *format, va_list args);
int snprintf(char *buffer, size_t size, const char *format, ...);
int vsnprintf(char *buffer, size_t size, const char *format, va_list args);
int vprintf(const char *format, va_list args);
#endif
