#ifndef DOOM_PORT_STRING_H
#define DOOM_PORT_STRING_H
#include <stddef.h>
char *strcpy(char *dest, const char *src);
char *strncpy(char *dest, const char *src, size_t count);
size_t strlen(const char *text);
int strcmp(const char *left, const char *right);
int strncmp(const char *left, const char *right, size_t count);
char *strcat(char *dest, const char *src);
char *strncat(char *dest, const char *src, size_t count);
char *strchr(const char *text, int ch);
char *strrchr(const char *text, int ch);
char *strstr(const char *text, const char *needle);
char *strdup(const char *text);
int strcasecmp(const char *left, const char *right);
int strncasecmp(const char *left, const char *right, size_t count);
char *strerror(int error);
void *memcpy(void *dest, const void *src, size_t count);
void *memmove(void *dest, const void *src, size_t count);
void *memset(void *dest, int value, size_t count);
int memcmp(const void *left, const void *right, size_t count);
#endif
