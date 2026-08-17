#ifndef STRING_H
#define STRING_H

#include <stddef.h>

char *strcpy(char *dst, char *src);
char *strncpy(char *dst, char *src, size_t n);
size_t strlen(char *str);
int strcmp(char *s1, char *s2);
int strncmp(char *s1, char *s2, size_t n);
char *strcat(char *dst, char *src);
char *strncat(char *dst, char *src, size_t n);
char *strchr(char *str, int c);
char *strrchr(char *str, int c);

void *memcpy(void *dst, void *src, size_t n);
void *memmove(void *dst, void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(void *s1, void *s2, size_t n);

#endif /* STRING_H */
