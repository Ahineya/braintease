/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "golf.h"
#include "../../libraries/ini/ini.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <errno.h>

static int integer(const char *s, long *out) {
    char *end;long n;errno=0;n=strtol(s,&end,10);
    if(errno||end==s||*end||n < -4096||n > 4096)return 0;
    *out=n;return 1;
}
static int pair(const char *s,double *x,double *y) {
    char *end;long a,b;errno=0;a=strtol(s,&end,10);
    if(errno||end==s||*end!=',')return 0;
    s=end+1;b=strtol(s,&end,10);
    if(errno||end==s||*end||a < -4096||a > 4096||b < -4096||b > 4096)return 0;
    *x=a;*y=b;return 1;
}
static int boolean(const char *s,int *out) {
    if(!strcmp(s,"true")){*out=1;return 1;}
    if(!strcmp(s,"false")){*out=0;return 1;}return 0;
}
void golf_free(GolfCourse *c) {
    int i;if(!c)return;
    for(i=0;i<GOLF_HOLES;i++){
        int j;for(j=0;j<c->holes[i].count;j++)free(c->holes[i].objects[j].comment);
        free(c->holes[i].objects);
    }
    free(c);
}
static GolfObject *append(GolfHole *h,int type) {
    GolfObject *p;int cap;
    if(h->count==GOLF_OBJECTS)return NULL;
    if(h->count==h->capacity){
        cap=h->capacity?h->capacity*2:16;
        p=realloc(h->objects,(unsigned long)cap*sizeof(*p));if(!p)return NULL;
        h->objects=p;h->capacity=cap;
    }
    p=h->objects+h->count++;memset(p,0,sizeof(*p));p->type=type;
    p->w=45;p->h=40;p->grade=4;p->interval=50;
    if(type==G_WATER)p->h=30;
    if(type==G_SLOPE){p->w=40;p->h=40;p->gradient=G_VERTICAL;}
    if(type==G_CUP)p->w=p->h=16;
    if(type==G_BUMPER)p->w=p->h=20;
    if(type==G_BLACKHOLE){p->w=16;p->h=18;p->exit_x=300;p->exit_y=100;p->min_speed=3;p->max_speed=5;}
    if(golf_rectangle(type)){p->w=80;p->h=40;}
    if(type==G_SIGN){p->w=110;p->walls=15;}
    if(type==G_WINDMILL){p->walls=G_LEFT|G_RIGHT;p->bottom=1;p->speed=5;}
    if(type==G_FLOATER){p->ax=p->ay=200;p->bx=p->by=100;return p;}
    p->ax=-15;p->ay=10;p->bx=15;p->by=-5;
    return p;
}
GolfCourse *golf_course(const char *text,unsigned long length,char *error,unsigned size) {
    GolfCourse *c=calloc(1,sizeof(*c));IniReader *r;
    GolfHole *h=NULL;GolfObject *o=NULL;
    char section[160]="",kind[32]="",type[32],xy[80],*at,*dash,*pipe;
    char name[160];const char *key,*v,*why="Invalid course";
    int i,n,status=0,valid,b;long number;double x,y;
    if(!c){snprintf(error,size,"Out of memory");return NULL;}
    strcpy(c->name,"Untitled course");
    for(i=0;i<GOLF_HOLES;i++){c->holes[i].par=3;c->holes[i].maxstrokes=10;c->holes[i].borders=1;}
    r=ini_reader_with_limits(text,length,262144UL,1024,16384UL);
    if(!r){golf_free(c);snprintf(error,size,"Out of memory");return NULL;}
    while((status=ini_next(r))==1){
        if(strcmp(section,ini_section(r))){
            if(strlen(ini_section(r))>=sizeof(section)){why="Section name too long";goto bad;}
            strcpy(section,ini_section(r));strcpy(name,section);
            dash=strchr(name,'-');at=strchr(name,'@');
            if(!dash||!at||at<=dash+1){why="Invalid course section";goto bad;}
            *dash=0;*at=0;
            if(!integer(name,&number)||number<0||number>GOLF_HOLES){why="Hole number outside 0..36";goto bad;}
            n=(int)number;
            if(strlen(dash+1)>=sizeof(type)||strlen(at+1)>=sizeof(xy)){why="Section name too long";goto bad;}
            strcpy(type,dash+1);strcpy(xy,at+1);pipe=strchr(xy,'|');if(pipe)*pipe=0;
            if(!pair(xy,&x,&y)){why="Invalid object position";goto bad;}
            strcpy(kind,type);h=NULL;o=NULL;
            if(n==0){if(strcmp(type,"course")){why="Invalid course metadata";goto bad;}}
            else {
                h=c->holes+n-1;if(n>c->count)c->count=n;
                if(!strcmp(type,"ball")){
                    if(h->has_ball){why="Duplicate ball start";goto bad;}
                    h->has_ball=1;h->start_x=x;h->start_y=y;
                }else if(!strcmp(type,"hole")){
                    if(h->has_info){why="Duplicate hole settings";goto bad;}h->has_info=1;
                }else{
                    i=!strcmp(type,"wall")?G_WALL:!strcmp(type,"cup")?G_CUP:
                        !strcmp(type,"sand")?G_SAND:!strcmp(type,"puddle")?G_WATER:
                        !strcmp(type,"slope")?G_SLOPE:!strcmp(type,"bumper")?G_BUMPER:
                        !strcmp(type,"blackhole")?G_BLACKHOLE:!strcmp(type,"bridge")?G_BRIDGE:
                        !strcmp(type,"floater")?G_FLOATER:!strcmp(type,"windmill")?G_WINDMILL:
                        !strcmp(type,"sign")?G_SIGN:-1;
                    if(i<0){snprintf(error,size,"Unsupported object: %s (hole %d)",type,n);goto done_bad;}
                    o=append(h,i);if(!o){why="Object budget or memory exhausted";goto bad;}
                    o->x=x;o->y=y;
                }
            }
        }
        key=ini_key(r);v=ini_value(r);valid=1;
        if(!h){
            if(!strcmp(key,"Name")||!strcmp(key,"name"))snprintf(c->name,sizeof(c->name),"%s",v);
            else if(!strcmp(key,"author"))snprintf(c->author,sizeof(c->author),"%s",v);
            else if(strncmp(key,"Name[",5))valid=0;
        }else if(!strcmp(key,"dummykey")){} /* KConfig's empty-section sentinel. */
        else if(!o){
            if(!strcmp(kind,"hole")){
                if(!strcmp(key,"par")||!strcmp(key,"maxstrokes")){
                    valid=integer(v,&number)&&number>=1&&number<=99;
                    if(valid){if(!strcmp(key,"par"))h->par=number;else h->maxstrokes=number;}
                }else if(!strcmp(key,"borderWalls"))valid=boolean(v,&h->borders);
                else if(!strcmp(key,"hasFinalLoad")){valid=boolean(v,&b);}
                else valid=0;
            }else valid=0;
        }else if(!strcmp(key,"startPoint")&&(o->type==G_WALL||o->type==G_FLOATER))valid=pair(v,&o->ax,&o->ay);
        else if(!strcmp(key,"endPoint")&&(o->type==G_WALL||o->type==G_FLOATER))valid=pair(v,&o->bx,&o->by);
        else if(!strcmp(key,"width")||!strcmp(key,"height")){
            valid=integer(v,&number)&&number>=1&&number<=4096;
            if(valid){if(!strcmp(key,"width"))o->w=number;else o->h=number;}
        }else if(!strcmp(key,"gradient")&&o->type==G_SLOPE){
            i=!strcmp(v,"Horizontal")?G_HORIZONTAL:!strcmp(v,"Vertical")?G_VERTICAL:
                !strcmp(v,"Diagonal")?G_DIAGONAL:!strcmp(v,"Opposite Diagonal")?G_CROSS:
                !strcmp(v,"Elliptic")?G_ELLIPTIC:-1;
            valid=i>=0;if(valid)o->gradient=i;
        }else if(!strcmp(key,"grade")&&o->type==G_SLOPE){valid=integer(v,&number)&&number>=0&&number<=8;if(valid)o->grade=number;}
        else if(!strcmp(key,"reversed")&&o->type==G_SLOPE)valid=boolean(v,&o->reversed);
        else if(!strcmp(key,"stuckOnGround")&&o->type==G_SLOPE)valid=boolean(v,&o->ground);
        else if(!strcmp(key,"changeEnabled")&&(o->type==G_SAND||o->type==G_WATER)){valid=boolean(v,&o->changing);}
        else if(!strcmp(key,"changeEvery")&&(o->type==G_SAND||o->type==G_WATER)){valid=integer(v,&number)&&number>=0;if(valid)o->interval=number;}
        else if(!strcmp(key,"speed")&&(o->type==G_FLOATER||o->type==G_WINDMILL)){
            valid=integer(v,&number)&&number>=0&&number<=100;if(valid)o->speed=number;
        }else if(!strcmp(key,"bottom")&&o->type==G_WINDMILL)valid=boolean(v,&o->bottom);
        else if(golf_rectangle(o->type)&&(!strcmp(key,"topWallVisible")||!strcmp(key,"leftWallVisible")||!strcmp(key,"rightWallVisible")||!strcmp(key,"botWallVisible"))){
            i=!strcmp(key,"topWallVisible")?G_TOP:!strcmp(key,"leftWallVisible")?G_LEFT:!strcmp(key,"rightWallVisible")?G_RIGHT:G_BOTTOM;
            valid=boolean(v,&b);if(valid){if(b)o->walls|=i;else o->walls&=~i;}
        }else if(!strcmp(key,"exit")&&o->type==G_BLACKHOLE)valid=pair(v,&o->exit_x,&o->exit_y);
        else if(!strcmp(key,"exitDeg")&&o->type==G_BLACKHOLE){valid=integer(v,&number);if(valid)o->exit_degrees=number;}
        else if((!strcmp(key,"minspeed")||!strcmp(key,"maxspeed"))&&o->type==G_BLACKHOLE){
            char *end;double value;errno=0;value=strtod(v,&end);
            valid=!errno&&end!=v&&!*end&&value>=0&&value<=100;
            if(valid){if(!strcmp(key,"minspeed"))o->min_speed=value;else o->max_speed=value;}
        }else if(!strcmp(key,"Comment")&&o->type==G_SIGN){
            free(o->comment);o->comment=malloc(strlen(v)+1);valid=o->comment!=NULL;
            if(valid){ /* KConfig string escapes, not INI continuations. */
                const char *src=v;char *dst=o->comment;
                while(*src){if(*src=='\\'&&src[1]){src++;*dst++=*src=='n'?'\n':*src=='t'?'\t':*src=='r'?'\r':*src=='s'?' ':*src;src++;}else *dst++=*src++;}*dst=0;
            }
        }else if(!strncmp(key,"Comment[",8)&&o->type==G_SIGN){} /* Use untranslated fallback. */
        else valid=0;
        if(!valid){snprintf(error,size,"Unsupported/invalid %s at line %lu",key,ini_line(r));goto done_bad;}
    }
    if(status<0){why=status==INI_LIMIT?"Course exceeds INI budgets":"Invalid INI syntax";goto bad;}
    if(!c->count){why="Course has no holes";goto bad;}
    for(i=0;i<c->count;i++)if(!golf_prepare(c->holes+i)){why="Missing start/cup, invalid walls or hole settings";goto bad;}
    ini_free(r);return c;
bad:
    snprintf(error,size,"%s (line %lu)",why,ini_line(r));
done_bad:
    ini_free(r);golf_free(c);return NULL;
}
