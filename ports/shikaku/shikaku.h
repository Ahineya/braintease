/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_SHIKAKU_H
#define RIPPLE_SHIKAKU_H
#define S_MAX 25
#define S_CELLS (S_MAX*S_MAX)
#define S_HISTORY 128
#define S_PER_SIZE 12
#define S_PUZZLES (6*S_PER_SIZE)
typedef struct { int x,y,w,h; } SRect;
typedef struct { int size,count; const char *data; } SPuzzle;
typedef struct { SRect *rects; int count; } SState;
typedef struct {
    int puzzle,size,clues[S_CELLS],cursor,states,generated;
    unsigned long seed;
    unsigned revision;
    SState history[S_HISTORY];
} SGame;
extern const SPuzzle s_puzzles[S_PUZZLES];
extern const int s_sizes[6];
void s_destroy(SGame *g);
int s_begin(SGame *g,int puzzle);
SState *s_state(SGame *g);
int s_inside(const SGame *g,SRect r);
int s_intersects(SRect a,SRect b);
/* 1: matches its single clue; 0: wrong area; -1: zero/multiple clues. */
int s_status(const SGame *g,SRect r);
int s_apply(SGame *g,SRect r,int erase);
int s_clear(SGame *g);
int s_undo(SGame *g);
int s_redo(SGame *g);
int s_restore(SGame *g,const SRect *r,int count);
int s_progress(SGame *g,int *errors);
int s_won(SGame *g);
SRect s_solution(int puzzle,int rectangle);
unsigned long s_signature(int puzzle);
unsigned long s_game_signature(const SGame *g);
typedef struct SGenerator SGenerator;
SGenerator *s_generate_start(int size,unsigned long seed);
void s_generate_free(SGenerator *g);
int s_generate_step(SGenerator *g,unsigned budget);
int s_generate_finish(SGenerator *generator,SGame *g);
unsigned long s_generate_nodes(SGenerator *g);
int s_generate_refinements(SGenerator *g);
int s_load(SGame *g,const char *path);
int s_save(SGame *g,const char *path);
#endif
