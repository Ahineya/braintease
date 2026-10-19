/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Host-only adversarial simulation: every original hole, deterministic shots. */
#include "../golf.h"
#include <stdio.h>
#include <stdlib.h>
#include <math.h>
static GolfGame game;
int main(int argc,char **argv){
    int arg,h,i,t,holes=0;unsigned long steps=0;char error[200];
    for(arg=1;arg<argc;arg++){
        FILE *f=fopen(argv[arg],"rb");long length;char *text;GolfCourse *c;
        if(!f)return 2;fseek(f,0,SEEK_END);length=ftell(f);rewind(f);text=malloc(length);
        if(!text||fread(text,1,length,f)!=(size_t)length)return 2;fclose(f);
        c=golf_course(text,length,error,sizeof(error));free(text);
        if(!c){fprintf(stderr,"%s: %s\n",argv[arg],error);return 1;}
        for(h=0;h<c->count;h++){
            golf_begin(&game,c->holes+h);holes++;
            for(i=0;i<12;i++){
                if(golf_finished(&game))golf_begin(&game,c->holes+h);
                if(game.state==G_READY)golf_shoot(&game,cos(i*2.399963),sin(i*2.399963),.25+i*.65);
                for(t=0;t<1200;t++){
                    golf_step(&game);steps++;
                    if(!isfinite(game.x)||!isfinite(game.y)||!isfinite(game.vx)||!isfinite(game.vy)||fabs(game.vx)>1000||fabs(game.vy)>1000){fprintf(stderr,"Invalid state %s hole %d step %d\n",argv[arg],h+1,t);return 1;}
                    if(game.state==G_HAZARD){if(!golf_recover(&game,G_DROP))golf_recover(&game,G_REHIT);break;}
                    if(golf_finished(&game)||(!golf_active(&game)))break;
                }
            }
        }
        golf_free(c);
    }
    printf("KOLF_SWEEP holes=%d steps=%lu failures=0\n",holes,steps);return holes==128?0:1;
}
