/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef RIPPLE_GOLF_H
#define RIPPLE_GOLF_H
#define GOLF_HOLES 36
#define GOLF_OBJECTS 256
enum GolfType { G_WALL, G_CUP, G_SAND, G_WATER, G_SLOPE,
    G_BUMPER, G_BLACKHOLE, G_BRIDGE, G_FLOATER, G_WINDMILL, G_SIGN };
enum GolfGradient { G_HORIZONTAL, G_VERTICAL, G_DIAGONAL, G_CROSS, G_ELLIPTIC };
enum GolfWalls { G_TOP=1, G_LEFT=2, G_RIGHT=4, G_BOTTOM=8 };
typedef struct GolfObject {
    int type, gradient, reversed, grade, changing, interval;
    int bounded,min_x,min_y,max_x,max_y;
    int walls,bottom,ground,speed;
    double x,y,w,h,ax,ay,bx,by,nx,ny,length;
    double exit_x,exit_y,exit_degrees,min_speed,max_speed;
    char *comment;
} GolfObject;
typedef struct GolfHole {
    GolfObject *objects;
    int count,capacity,par,maxstrokes,borders,has_ball,has_info;
    double start_x,start_y;
} GolfHole;
typedef struct GolfCourse {
    char name[80],author[80];
    int count;
    GolfHole holes[GOLF_HOLES];
} GolfCourse;
enum GolfState { G_READY, G_ROLLING, G_HOLED, G_LIMIT, G_WARPING, G_HAZARD };
typedef struct GolfLive {
    GolfObject object;
    double progress,dx,dy;
    int carrier,level,visible,direction,frame,runs;
} GolfLive;
/* One pre-shot checkpoint, as in Kolf. Storage is owned by the game and only
 * the live course's objects are copied; simulation never allocates. */
typedef struct GolfSnapshot {
    double x,y,vx,vy,bumper_max,warp_speed;
    unsigned long ticks;
    int state,strokes,penalties,warp,warp_ticks,warps;
    GolfLive live[GOLF_OBJECTS];
} GolfSnapshot;
typedef struct GolfGame {
    const GolfHole *hole;
    double x,y,vx,vy,shot_x,shot_y,bumper_max,warp_speed;
    unsigned long ticks,revision;
    int state,strokes,penalties,event,animated,warp,warp_ticks,warps;
    GolfLive live[GOLF_OBJECTS];
    int can_undo,hazard;
    double hazard_dx,hazard_dy;
    GolfSnapshot before;
} GolfGame;
enum GolfEvent { G_NONE, G_HIT, G_BOUNCE, G_SPLASH, G_SUNK, G_OUT, G_WARP, G_BUMP, G_UNDONE, G_RECOVERED };
enum GolfRecovery { G_DROP, G_REHIT };
GolfCourse *golf_course(const char *text, unsigned long length, char *error, unsigned size);
void golf_free(GolfCourse *course);
int golf_prepare(GolfHole *hole);
void golf_begin(GolfGame *game, const GolfHole *hole);
int golf_shoot(GolfGame *game, double dx, double dy, double strength);
int golf_undo(GolfGame *game);
/* A failed drop leaves the modal hazard state and score unchanged. */
int golf_recover(GolfGame *game,int choice);
void golf_step(GolfGame *game); /* One fixed 11 ms logical step. No I/O/allocation. */
int golf_contains(const GolfObject *object, double x, double y);
int golf_rectangle(int type);
int golf_finished(const GolfGame *game);
int golf_active(const GolfGame *game);
/* Rectangle edges and moving gate, shared by renderer and collision solver. */
int golf_edges(const GolfLive *item, GolfObject *edges);
#endif
