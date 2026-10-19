/* SPDX-License-Identifier: GPL-3.0-or-later */
#ifndef RIPPLE_KLOTSKI_H
#define RIPPLE_KLOTSKI_H
#define K_LEVELS 28
#define K_CELLS (19*22)
#define K_HISTORY 16384
typedef struct { const char *name; int group, width, height; const char *map; } KLevel;
typedef struct { unsigned long moves; char last; int dx,dy; } KScore;
typedef struct { char id; int dx,dy; KScore before; } KStep;
typedef struct {
    const KLevel *level;
    char board[K_CELLS];
    KScore score;
    KStep *steps;
    unsigned count,cursor,capacity;
} KGame;
extern const KLevel k_levels[K_LEVELS];
int k_begin(KGame *, const KLevel *);
void k_free(KGame *);
int k_static(char);
char k_at(const KGame *,int,int);
int k_can(const KGame *,char,int,int);
/* 1 moved; 0 illegal; -1 allocation/history limit. No change on failure. */
int k_move(KGame *,char,int,int);
int k_undo(KGame *);
int k_redo(KGame *);
int k_won(const KGame *);
int k_tile(const KGame *,int,int);
/* Text journal: validated by replay before replacing the live game. */
int k_save(const KGame *,const char *,unsigned long);
/* 1 loaded; 0 missing; -1 invalid/I/O. Live game unchanged on failure. */
int k_load(KGame *,const char *,unsigned long *);
unsigned long k_best(const KLevel *,const char *);
int k_flush(int);
#endif
