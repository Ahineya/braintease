#ifndef NETHACK_RIPPLE_SYSTEM_H
#define NETHACK_RIPPLE_SYSTEM_H
#include <sys/types.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <signal.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
int nh_fscanf(FILE *, const char *, ...);
#define fscanf nh_fscanf
int tgetent(char *, const char *);
int tgetflag(const char *);
int tgetnum(const char *);
char *tgetstr(const char *, char **);
int tputs(const char *, int, int (*)(int));
char *tgoto(const char *, int, int);
void append_slash(char *);
void ripple_nethack_paths(const char **, const char *);
#define SIG_RET_TYPE void (*)(int)
long lrand48(void);
void srand48(long);
#endif
