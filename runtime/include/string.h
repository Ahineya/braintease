#ifndef STRING_H
#define STRING_H

// String manipulation functions
char* strcpy(char* dst, char* src);
char* strncpy(char* dst, char* src, int n);
int strlen(char* str);
int strcmp(char* s1, char* s2);
int strncmp(char* s1, char* s2, int n);
char* strcat(char* dst, char* src);
char* strncat(char* dst, char* src, int n);
char* strchr(char* str, int c);
char* strrchr(char* str, int c);

// Memory functions (often in string.h)
void* memcpy(void* dst, void* src, unsigned int n);
void* memmove(void* dst, void* src, unsigned int n);
void* memset(void* dst, int c, unsigned int n);
int memcmp(void* s1, void* s2, unsigned int n);

#endif // STRING_H