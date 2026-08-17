#ifndef STDLIB_H
#define STDLIB_H

// Standard library functions

// Common macros
#define NULL ((void *)0)

// Exit codes
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

// Random number generation
#define RAND_MAX 0x7FFF  // Maximum value returned by rand() (32767)

int rand(void);
void srand(unsigned int seed);

// Memory allocation functions
void *malloc(unsigned int size);
void free(void *ptr);
void *calloc(unsigned int nmemb, unsigned int size);
void *realloc(void *ptr, unsigned int size);

// TODO: Future additions
// void exit(int status);
// int abs(int j);

#endif // STDLIB_H