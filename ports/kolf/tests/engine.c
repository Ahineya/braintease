/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../golf.h"
#include "../putting.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
static int checks,failures;
#define CHECK(x) do {checks++;if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);failures++;}}while(0)
static GolfHole flat;
static GolfObject objects[8];
static void reset(void) {
    memset(&flat,0,sizeof(flat));memset(objects,0,sizeof(objects));
    flat.objects=objects;flat.has_info=flat.has_ball=1;flat.start_x=100;flat.start_y=200;
    flat.borders=1;flat.par=2;flat.maxstrokes=10;flat.count=1;
    objects[0].type=G_CUP;objects[0].x=350;objects[0].y=350;objects[0].w=objects[0].h=12;
}
static double travel(int sand) {
    static GolfGame g;int i;reset();flat.start_x=30;
    if(sand){flat.count=2;objects[1].type=G_SAND;objects[1].x=200;objects[1].y=200;objects[1].w=objects[1].h=1000;}
    golf_begin(&g,&flat);CHECK(golf_shoot(&g,1,0,2));
    for(i=0;i<1000&&g.state==G_ROLLING;i++)golf_step(&g);
    CHECK(g.state==G_READY&&i<1000);return g.x;
}
static void physics(void) {
    static GolfGame g;GolfObject *o;int i;double grass=travel(0),sand=travel(1);
    CHECK(grass>160&&grass<190);CHECK(sand>45&&sand<60);CHECK(grass>sand+100);
    reset();flat.count=2;o=objects+1;o->type=G_WALL;o->ax=150;o->ay=0;o->bx=150;o->by=400;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);g.x=144;CHECK(golf_shoot(&g,1,0,8));golf_step(&g);
    CHECK(g.vx<0&&g.x<146&&g.x>130);CHECK(fabs(g.vy)<1e-10);
    /* Circle-end contact, rather than an infinite-line approximation. */
    reset();flat.count=2;o=objects+1;o->type=G_WALL;o->ax=150;o->ay=200;o->bx=150;o->by=300;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);g.x=144;g.y=198;golf_shoot(&g,1,0,8);golf_step(&g);
    CHECK(g.vx<0&&g.vy<0&&g.x<150);
    /* A diagonal hit must exchange x momentum for y momentum. */
    reset();flat.count=2;o=objects+1;o->type=G_WALL;o->ax=100;o->ay=100;o->bx=200;o->by=200;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);g.x=140;g.y=150;golf_shoot(&g,1,0,8);golf_step(&g);
    CHECK(fabs(g.vx)<0.001&&g.vy>7);
    reset();golf_begin(&g,&flat);g.x=395;golf_shoot(&g,1,0,8);golf_step(&g);CHECK(g.vx<0&&g.x<396);
    reset();flat.borders=0;golf_begin(&g,&flat);g.x=399;golf_shoot(&g,1,0,8);golf_step(&g);
    CHECK(g.state==G_READY&&g.x==399&&g.strokes==1&&g.penalties==0&&g.event==G_OUT);
    reset();flat.count=2;o=objects+1;o->type=G_WATER;o->x=110;o->y=200;o->w=o->h=12;
    golf_begin(&g,&flat);golf_shoot(&g,1,0,5);golf_step(&g);
    CHECK(g.state==G_HAZARD&&g.x==105&&g.strokes==2&&g.penalties==1&&g.event==G_SPLASH);
    CHECK(golf_recover(&g,G_REHIT)&&g.state==G_READY&&g.x==100&&g.strokes==2);
    reset();objects[0].x=103;objects[0].y=200;golf_begin(&g,&flat);golf_shoot(&g,1,0,2);golf_step(&g);
    CHECK(g.state==G_HOLED&&g.x==103&&g.vx==0&&g.strokes==1);
    golf_begin(&g,&flat);golf_shoot(&g,1,0,6);golf_step(&g);CHECK(g.state==G_ROLLING);
    CHECK(!golf_shoot(&g,1,0,2));
    reset();flat.maxstrokes=1;golf_begin(&g,&flat);golf_shoot(&g,1,0,0.25);
    for(i=0;i<100&&g.state==G_ROLLING;i++)golf_step(&g);
    CHECK(g.state==G_LIMIT);CHECK(!golf_shoot(&g,1,0,2));
    reset();flat.count=2;o=objects+1;o->type=G_SLOPE;o->x=0;o->y=0;o->w=o->h=400;
    o->grade=4;o->gradient=G_VERTICAL;o->reversed=1;
    golf_begin(&g,&flat);golf_shoot(&g,1,0,1);golf_step(&g);CHECK(g.vy>0.05&&g.vy<0.06);
    o->gradient=G_DIAGONAL;o->w=o->h=100;
    CHECK(golf_contains(o,10,10)&&!golf_contains(o,90,90));
    o->reversed=0;CHECK(!golf_contains(o,10,10)&&golf_contains(o,90,90));
    o->gradient=G_CROSS;CHECK(golf_contains(o,10,90)&&!golf_contains(o,90,10));
    o->gradient=G_ELLIPTIC;CHECK(golf_contains(o,50,50)&&!golf_contains(o,1,1));
}
static void advanced(void) {
    static GolfGame g;GolfObject *o;int i;double old;
    /* 4 is Kolf's Unlimited sentinel, not a four-stroke limit. */
    reset();flat.maxstrokes=4;golf_begin(&g,&flat);g.strokes=7;golf_shoot(&g,1,0,.25);
    for(i=0;i<100&&g.state==G_ROLLING;i++)golf_step(&g);
    CHECK(g.state==G_READY&&g.strokes==8);
    reset();flat.count=2;o=objects+1;o->type=G_BUMPER;o->x=118;o->y=200;o->w=o->h=20;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,5);golf_step(&g);
    CHECK(g.event==G_BUMP&&g.vx< -5&&g.bumper_max<8);
    reset();flat.count=3;o=objects+1;o->type=G_WATER;o->x=100;o->y=200;o->w=o->h=100;
    o=objects+2;o->type=G_BRIDGE;o->x=80;o->y=180;o->w=100;o->h=40;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,2);golf_step(&g);
    CHECK(g.state==G_ROLLING&&g.penalties==0&&g.vx>1.98);
    g.x=195;g.y=200;g.vx=-3;golf_step(&g);CHECK(g.event!=G_SPLASH);
    /* Ground-forced slopes do not accelerate a ball on a bridge. */
    flat.count=4;o=objects+3;o->type=G_SLOPE;o->x=80;o->y=180;o->w=100;o->h=40;o->grade=8;o->gradient=G_VERTICAL;o->ground=1;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,2);golf_step(&g);CHECK(g.vy==0);
    o->ground=0;golf_begin(&g,&flat);golf_shoot(&g,1,0,2);golf_step(&g);CHECK(g.vy<-.1);
    /* Floater positions come from absolute endpoints and carry cups and ball. */
    reset();flat.count=2;objects[0].x=120;objects[0].y=200;
    o=objects+1;o->type=G_FLOATER;o->ax=80;o->ay=180;o->bx=100;o->by=180;o->w=80;o->h=40;o->speed=7;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);CHECK(golf_active(&g));golf_step(&g);
    CHECK(g.state==G_READY&&fabs(g.x-102)<.001&&fabs(g.live[0].object.x-122)<.001);
    for(i=0;i<21;i++)golf_step(&g);
    CHECK(g.live[1].object.x>=80&&g.live[1].object.x<=100&&g.penalties==0);
    /* Blink timing freezes the collision surface while it is invisible. */
    reset();flat.count=2;o=objects+1;o->type=G_WATER;o->x=110;o->y=200;o->w=o->h=12;o->changing=1;o->interval=0;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);for(i=0;i<35;i++)golf_step(&g);
    CHECK(!g.live[1].visible);golf_shoot(&g,1,0,5);golf_step(&g);CHECK(g.penalties==0);
    /* Warp hides/stops the ball until transit completes, then ejects directionally. */
    reset();flat.count=2;o=objects+1;o->type=G_BLACKHOLE;o->x=106;o->y=200;o->w=16;o->h=18;
    o->exit_x=250;o->exit_y=200;o->min_speed=2;o->max_speed=4;o->exit_degrees=90;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,2);golf_step(&g);
    CHECK(g.state==G_WARPING&&g.event==G_WARP&&g.warp_ticks>0);
    for(i=0;i<200&&g.state==G_WARPING;i++)golf_step(&g);
    CHECK(g.state==G_ROLLING&&fabs(g.x-250)<.001&&fabs(g.y-190)<.001&&g.vy< -3&&g.warps==1);
    /* Rectangle wall flags must produce actual collisions, not decoration. */
    reset();flat.count=2;o=objects+1;o->type=G_SIGN;o->x=110;o->y=180;o->w=80;o->h=40;o->walls=G_LEFT;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,8);golf_step(&g);CHECK(g.vx<0);
    /* Gate movement continues without a shot, and pushes a resting ball. */
    reset();flat.count=2;o=objects+1;o->type=G_WINDMILL;o->x=80;o->y=156;o->w=80;o->h=40;o->bottom=1;o->speed=9;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);g.x=134;g.y=200;old=g.live[1].progress;
    golf_step(&g);CHECK(g.live[1].progress>old);CHECK(g.state==G_ROLLING&&g.vx>0);
    /* Retry resets all animation and warp state. */
    golf_begin(&g,&flat);CHECK(g.ticks==0&&g.state==G_READY&&g.warps==0&&g.strokes==0);
}
static const char simple[]="[0-course@-50,-50]\nName=Test\n[1-ball@100,200]\nx=y\n[1-cup@150,200|1]\ndummykey=true\n[1-hole@-50,-50|0]\npar=2\nmaxstrokes=10\nborderWalls=true\n";
static void recovery(void){
    static GolfGame g;static GolfSnapshot saved;static GolfObject many[GOLF_OBJECTS];
    GolfObject *o;int i;double x,y,progress;unsigned long ticks,revision;
    reset();golf_begin(&g,&flat);CHECK(!golf_undo(&g)&&!golf_recover(&g,G_DROP));
    CHECK(golf_shoot(&g,1,0,2));golf_step(&g);CHECK(g.x>100&&g.can_undo);
    CHECK(golf_undo(&g)&&g.state==G_READY&&g.x==100&&g.strokes==0&&g.penalties==0);
    CHECK(golf_undo(&g)&&g.strokes==0); /* One reusable last-shot checkpoint. */
    CHECK(!golf_shoot(&g,0,0,2)&&g.can_undo&&g.before.x==100);
    /* Floater motion, carried objects, blink phase and replay timing rewind. */
    reset();flat.count=3;o=objects+1;o->type=G_FLOATER;o->ax=80;o->ay=180;o->bx=140;o->by=180;o->w=80;o->h=40;o->speed=7;
    objects[0].x=120;objects[0].y=200;
    o=objects+2;o->type=G_SAND;o->x=300;o->y=300;o->w=o->h=20;o->changing=1;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);for(i=0;i<7;i++)golf_step(&g);
    CHECK(golf_shoot(&g,-1,0,1.5));memcpy(&saved,&g.before,sizeof(saved));
    for(i=0;i<17;i++)golf_step(&g);x=g.x;y=g.y;progress=g.live[1].progress;ticks=g.ticks;revision=g.revision;
    CHECK(golf_undo(&g)&&g.ticks==saved.ticks&&g.revision>revision);
    CHECK(g.x==saved.x&&g.y==saved.y&&g.strokes==saved.strokes&&memcmp(g.live,saved.live,3*sizeof(*g.live))==0);
    CHECK(golf_shoot(&g,-1,0,1.5));for(i=0;i<17;i++)golf_step(&g);
    CHECK(g.x==x&&g.y==y&&g.ticks==ticks&&g.live[1].progress==progress);
    /* Out of bounds rewinds the full scene, charging the played stroke only. */
    reset();flat.borders=0;flat.start_x=399;flat.count=2;
    o=objects+1;o->type=G_FLOATER;o->ax=80;o->ay=80;o->bx=140;o->by=80;o->w=40;o->h=40;o->speed=7;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);CHECK(golf_shoot(&g,1,0,8));memcpy(&saved,&g.before,sizeof(saved));golf_step(&g);
    CHECK(g.event==G_OUT&&g.x==399&&g.strokes==1&&g.penalties==0&&g.ticks==saved.ticks);
    CHECK(memcmp(g.live,saved.live,2*sizeof(*g.live))==0&&golf_undo(&g)&&g.strokes==0);
    /* A water decision freezes animation and charges its penalty just once. */
    reset();flat.count=3;o=objects+1;o->type=G_WATER;o->x=110;o->y=200;o->w=o->h=12;
    o=objects+2;o->type=G_FLOATER;o->ax=80;o->ay=80;o->bx=140;o->by=80;o->w=40;o->h=40;o->speed=7;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,5);golf_step(&g);
    CHECK(g.state==G_HAZARD&&!golf_active(&g)&&g.strokes==2&&g.penalties==1);
    x=g.live[2].object.x;ticks=g.ticks;for(i=0;i<10;i++)golf_step(&g);
    CHECK(g.ticks==ticks&&g.live[2].object.x==x&&!golf_recover(&g,99));
    CHECK(golf_recover(&g,G_REHIT)&&g.x==100&&g.live[2].object.x==x&&g.strokes==2);
    CHECK(golf_undo(&g)&&g.strokes==0&&g.penalties==0&&g.live[2].object.x==80);
    /* Drop backs out in 3px steps until the entire ball clears the hazard. */
    reset();flat.start_x=80;flat.count=2;o=objects+1;o->type=G_WATER;o->x=110;o->y=200;o->w=o->h=30;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,2);
    for(i=0;i<30&&g.state!=G_HAZARD;i++)golf_step(&g);
    CHECK(g.state==G_HAZARD&&golf_recover(&g,G_DROP));
    CHECK(g.x>80&&g.x<91&&g.y==200&&g.strokes==2&&g.penalties==1&&g.state==G_READY);
    CHECK(!golf_recover(&g,G_DROP)&&g.strokes==2);
    CHECK(golf_undo(&g)&&g.x==80&&g.strokes==0&&g.penalties==0);
    /* The drop must also avoid another visible, overlapping puddle. */
    golf_shoot(&g,1,0,2);for(i=0;i<30&&g.state!=G_HAZARD;i++)golf_step(&g);
    g.live[0].object=g.live[1].object;g.live[0].object.x=95;
    CHECK(golf_recover(&g,G_DROP)&&g.x<76&&g.strokes==2);
    /* A blinking puddle appearing under a stationary ball has no entry vector. */
    reset();flat.count=2;o=objects+1;o->type=G_WATER;o->x=100;o->y=200;o->w=o->h=20;o->changing=1;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_step(&g);
    CHECK(g.state==G_HAZARD&&!g.can_undo&&golf_recover(&g,G_DROP));
    CHECK(g.x>114&&g.y==200&&g.strokes==1&&g.penalties==1);
    /* Invalid courses with no dry position cannot hang or silently lose score. */
    reset();flat.count=2;flat.maxstrokes=1;o=objects+1;o->type=G_WATER;o->x=o->y=200;o->w=o->h=2000;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,2);golf_step(&g);
    CHECK(g.state==G_HAZARD&&!golf_recover(&g,G_DROP)&&g.state==G_HAZARD&&g.strokes==2);
    CHECK(golf_recover(&g,G_REHIT)&&g.state==G_LIMIT&&g.strokes==2);
    CHECK(golf_undo(&g)&&g.state==G_READY&&g.strokes==0);
    /* Undo also restores a ball hidden in a warp or already holed. */
    reset();flat.count=2;o=objects+1;o->type=G_BLACKHOLE;o->x=106;o->y=200;o->w=16;o->h=18;o->exit_x=250;o->exit_y=200;o->min_speed=2;o->max_speed=4;
    CHECK(golf_prepare(&flat));golf_begin(&g,&flat);golf_shoot(&g,1,0,2);golf_step(&g);
    CHECK(g.state==G_WARPING&&golf_undo(&g)&&g.state==G_READY&&g.warp==-1&&g.warps==0&&g.strokes==0);
    reset();objects[0].x=102;objects[0].y=200;golf_begin(&g,&flat);golf_shoot(&g,1,0,2);golf_step(&g);
    CHECK(g.state==G_HOLED&&golf_undo(&g)&&g.state==G_READY&&g.x==100&&g.strokes==0);
    /* Maximum-size snapshots exercise the guest's cross-bank memory copies. */
    reset();memset(many,0,sizeof(many));flat.objects=many;flat.count=GOLF_OBJECTS;
    for(i=0;i<GOLF_OBJECTS;i++){many[i]=objects[0];many[i].x=350;many[i].y=350;}
    golf_begin(&g,&flat);g.live[GOLF_OBJECTS-1].runs=17;golf_shoot(&g,1,0,1);
    g.live[0].visible=0;g.live[GOLF_OBJECTS-1].runs=99;g.live[GOLF_OBJECTS-1].object.x=12;
    CHECK(golf_undo(&g)&&g.live[0].visible&&g.live[GOLF_OBJECTS-1].runs==17&&g.live[GOLF_OBJECTS-1].object.x==350);
    golf_begin(&g,&flat);CHECK(!g.can_undo&&!golf_undo(&g));
}
static void parser(void) {
    GolfCourse *c;char e[160];const char *bad;
    /* Unknown properties are not silently accepted. */
    c=golf_course(simple,sizeof(simple)-1,e,sizeof(e));CHECK(!c&&strstr(e,"x at line"));golf_free(c);
    bad="[1-ball@100,200]\ndummykey=true\n[1-cup@150,200|1]\ndummykey=true\n[1-hole@-50,-50|0]\npar=2\n";
    c=golf_course(bad,strlen(bad),e,sizeof(e));CHECK(c&&c->count==1&&c->holes[0].count==1);golf_free(c);
    bad="[37-ball@10,20]\ndummykey=true\n";c=golf_course(bad,strlen(bad),e,sizeof(e));CHECK(!c&&strstr(e,"number"));
    bad="[1-unknown@10,20|1]\ndummykey=true\n";c=golf_course(bad,strlen(bad),e,sizeof(e));CHECK(!c&&strstr(e,"Unsupported object: unknown"));
    bad="[1-cup@10,20|1]\ndummykey=true\n";c=golf_course(bad,strlen(bad),e,sizeof(e));CHECK(!c&&strstr(e,"Missing"));
    bad="[1-ball@10,20]\ndummykey=true\n[1-slope@0,0|1]\nwidth=0\n";
    c=golf_course(bad,strlen(bad),e,sizeof(e));CHECK(!c&&strstr(e,"width"));
    bad="[1-sand@10,20|1]\nchangeEnabled=maybe\n";c=golf_course(bad,strlen(bad),e,sizeof(e));CHECK(!c&&strstr(e,"changeEnabled"));
    c=golf_course("x=a\0b",5,e,sizeof(e));CHECK(!c);
}
static void putting(void){
    GolfPutt p,q;int i;double early;
    putt_init(&p,0);CHECK(p.phase==PUTT_IDLE&&p.pull==9);
    putt_press(&p,0);putt_update(&p,199);CHECK(p.phase==PUTT_CHARGE&&p.strength==13.5&&p.pull==18);
    early=putt_power(&p);putt_release(&p,200);CHECK(p.phase==PUTT_RETURN&&putt_power(&p)>early);
    putt_update(&p,1000);CHECK(p.phase==PUTT_FIRE&&fabs(putt_power(&p)-1.875)<.00001);
    /* Chunking/render cadence does not change the swing. */
    putt_init(&p,0);putt_init(&q,0);putt_press(&p,0);putt_press(&q,0);
    for(i=1;i<=600;i++)putt_update(&p,i);putt_update(&q,600);
    CHECK(p.pull==q.pull&&p.strength==q.strength&&p.phase==q.phase);
    putt_release(&p,600);putt_release(&q,600);putt_update(&p,1000);putt_update(&q,1000);
    CHECK(p.phase==PUTT_FIRE&&q.phase==PUTT_FIRE&&putt_power(&p)==putt_power(&q));
    putt_init(&p,0);putt_press(&p,0);putt_update(&p,2000);
    CHECK(p.phase==PUTT_FIRE&&p.strength==55.5&&fabs(putt_power(&p)-6.9375)<.00001);
    putt_cancel(&p);putt_release(&p,2010);putt_update(&p,3000);CHECK(p.phase==PUTT_IDLE);
    putt_press(&p,4000);putt_release(&p,4001);putt_update(&p,5000);CHECK(p.phase==PUTT_FIRE&&putt_power(&p)==1);
    putt_init(&p,1);putt_press(&p,0);putt_release(&p,1);putt_update(&p,400);
    CHECK(p.phase==PUTT_CHARGE&&p.strength>14&&p.gauge>0);
    putt_press(&p,400);CHECK(p.phase==PUTT_PRECISION);
    for(i=402;i<2000&&p.gauge>1;i+=2)putt_update(&p,i);
    CHECK(p.phase==PUTT_PRECISION&&fabs(p.gauge)<2);
    putt_press(&p,i);CHECK(p.phase==PUTT_RETURN&&p.correction==0);
    putt_update(&p,3000);CHECK(p.phase==PUTT_FIRE&&putt_power(&p)>1);
    putt_init(&p,1);putt_press(&p,0);putt_update(&p,500);early=p.strength;putt_press(&p,500);putt_press(&p,501);
    CHECK(p.phase==PUTT_RETURN&&p.correction>=-45&&p.correction<45&&p.strength<=early);
    putt_init(&p,1);putt_press(&p,0);putt_update(&p,10000);CHECK(p.phase==PUTT_IDLE);
    /* Deadlines work over the guest's 32-bit monotonic clock rollover. */
    putt_init(&p,0);putt_press(&p,~0UL-10);putt_update(&p,10);CHECK(p.strength==1.5);
    reset();{static GolfGame g;golf_begin(&g,&flat);CHECK(golf_shoot(&g,1,0,8.25));}
}
int main(int argc,char **argv) {
    FILE *f;long len;char *s,e[160];GolfCourse *c;int i,arg;
    physics();parser();advanced();putting();recovery();
    for(arg=1;arg<argc;arg++){
        f=fopen(argv[arg],"rb");if(!f)return 2;fseek(f,0,SEEK_END);len=ftell(f);rewind(f);s=malloc(len);if(!s)return 2;
        if(fread(s,1,len,f)!=(size_t)len)return 2;fclose(f);c=golf_course(s,len,e,sizeof(e));free(s);
        CHECK(c!=NULL);if(!c){puts(e);return 1;}
        printf("KOLF_COURSE_OK holes=%d name=%s\n",c->count,c->name);
        for(i=0;i<c->count;i++){CHECK(c->holes[i].has_ball&&c->holes[i].par>=1);}
        golf_free(c);
    }
    printf("KOLF_ENGINE checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
