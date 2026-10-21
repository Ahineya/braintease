/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "../shikaku.h"
static SGame game,loaded;
static unsigned long checks,failures;
#define CHECK(c) do{checks++;if(!(c)){failures++;printf("FAIL line %d\n",__LINE__);}}while(0)
int main(void) {
    int p,i,j,k,area,bad,result,loops,max,big,ones;unsigned long signature;SGenerator *gen;SRect r,other;FILE *f;char path[100];
    snprintf(path,sizeof(path),"/tmp/shikaku-check-%d",(int)getpid());
    for(p=0;p<S_PUZZLES;p++){
        CHECK(s_begin(&game,p));CHECK(game.size==s_sizes[p/S_PER_SIZE]);CHECK(!s_won(&game));area=0;
        for(i=0;i<s_puzzles[p].count;i++){
            r=s_solution(p,i);CHECK(s_inside(&game,r));CHECK(s_status(&game,r)==1);
            for(j=0;j<i;j++)CHECK(!s_intersects(r,s_solution(p,j)));
            CHECK(s_apply(&game,r,0)==1);area+=r.w*r.h;CHECK(s_progress(&game,&bad)==area&&bad==0);
        }
        CHECK(area==game.size*game.size);CHECK(s_won(&game));
        CHECK(s_undo(&game));CHECK(!s_won(&game));CHECK(s_redo(&game));CHECK(s_won(&game));
        CHECK(s_apply(&game,r,0)==0);CHECK(s_won(&game));
        other.x=0;other.y=0;other.w=other.h=game.size;
        CHECK(s_apply(&game,other,0)==1);CHECK(s_state(&game)->count==1);CHECK(!s_won(&game));
        CHECK(s_undo(&game));CHECK(s_won(&game));
        CHECK(s_apply(&game,r,1)==1);CHECK(!s_won(&game));CHECK(!s_redo(&game));
        CHECK(s_undo(&game));CHECK(s_won(&game));CHECK(s_clear(&game));CHECK(!s_state(&game)->count);
        CHECK(s_undo(&game));CHECK(s_won(&game));
    }
    CHECK(s_begin(&game,0));CHECK(!s_begin(&game,-1));CHECK(!s_begin(&game,S_PUZZLES));
    r.x=-1;r.y=0;r.w=r.h=1;CHECK(!s_apply(&game,r,0));r.x=0;r.w=32767;CHECK(!s_apply(&game,r,0));
    r.w=1;r.h=0;CHECK(!s_apply(&game,r,0));CHECK(!s_undo(&game));CHECK(!s_redo(&game));
    r=s_solution(0,0);CHECK(s_apply(&game,r,0));CHECK(s_save(&game,path)==0);CHECK(s_begin(&loaded,0));CHECK(s_load(&loaded,path)==1);
    CHECK(s_state(&loaded)->count==1);CHECK(!memcmp(s_state(&loaded)->rects,&r,sizeof(r)));
    CHECK(s_begin(&loaded,S_PER_SIZE));CHECK(s_load(&loaded,path)==-1);CHECK(!s_state(&loaded)->count);
    CHECK(s_begin(&loaded,0));CHECK(s_load(&loaded,path)==1);
    f=fopen(path,"w");CHECK(f!=NULL);if(f){fprintf(f,"SHIKAKU 1\n0 %lu 1\n99 0 1 1\n",s_signature(0));fclose(f);}
    CHECK(s_load(&loaded,path)==-1);CHECK(s_state(&loaded)->count==1);
    f=fopen(path,"w");if(f){fprintf(f,"SHIKAKU 1\n0 %lu 2\n0 0 2 2\n1 1 1 1\n",s_signature(0));fclose(f);}
    CHECK(s_load(&loaded,path)==-1);CHECK(s_state(&loaded)->count==1);
    f=fopen(path,"w");if(f){fprintf(f,"SHIKAKU 1\n0 %lu 0\ngarbage\n",s_signature(0));fclose(f);}
    CHECK(s_load(&loaded,path)==-1);
    f=fopen(path,"w");if(f){fputs("SHIKAKU 1\n99999999999999999999999999999999999999999\n",f);fclose(f);}
    CHECK(s_load(&loaded,path)==-1);CHECK(s_state(&loaded)->count==1);unlink(path);CHECK(s_load(&loaded,path)==0);
    CHECK(s_begin(&game,0));r.x=r.y=0;r.w=r.h=1;
    for(i=0;i<S_HISTORY+40;i++){CHECK(s_apply(&game,r,0)==1);CHECK(s_apply(&game,r,1)==1);}
    CHECK(game.states==S_HISTORY);for(i=0;i<S_HISTORY-1;i++)CHECK(s_undo(&game));CHECK(!s_undo(&game));
    CHECK(s_generate_start(26,1)==NULL);CHECK(s_generate_start(5,0)==NULL);
    for(k=0;k<6;k++)for(j=1;j<=3;j++){
        signature=0;
        for(p=0;p<2;p++){
            gen=s_generate_start(s_sizes[k],j);CHECK(gen!=NULL);if(!gen)continue;
            loops=0;do{result=s_generate_step(gen,64);loops++;}while(!result&&loops<20000);
            CHECK(result==1);CHECK(s_generate_finish(gen,&game));s_generate_free(gen);
            CHECK(game.generated&&game.seed==(unsigned long)j&&game.size==s_sizes[k]);
            area=max=big=ones=0;for(i=0;i<game.size*game.size;i++){
                area+=game.clues[i];if(game.clues[i]>max)max=game.clues[i];if(game.clues[i]>=12)big++;if(game.clues[i]==1)ones++;
            }CHECK(area==game.size*game.size);
            if(game.size>=15){CHECK(max>=18);CHECK(big>=2);}
            if(game.size>=10){int count=0;for(i=0;i<game.size*game.size;i++)if(game.clues[i])count++;CHECK(ones<=count/8);}
            if(p)CHECK(signature==s_game_signature(&game));else signature=s_game_signature(&game);
            if(getenv("SHIKAKU_DUMP")&&!p){printf("SHIKAKU_CLUES %d %d",game.size,j);for(i=0;i<game.size*game.size;i++)printf(" %d",game.clues[i]);putchar('\n');}
        }
        r.x=r.y=0;r.w=r.h=1;CHECK(s_apply(&game,r,0));CHECK(s_save(&game,path)==0);
        CHECK(s_clear(&game));CHECK(s_load(&game,path)==1);CHECK(s_state(&game)->count==1);unlink(path);
        printf("SHIKAKU_GENERATED %d %d %lu\n",game.size,j,signature);
    }
    s_destroy(&game);s_destroy(&loaded);printf("SHIKAKU_CHECK checks=%lu failures=%lu\n",checks,failures);return failures?1:0;
}
