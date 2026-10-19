/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "../klotski.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int failures,checks;
#define CHECK(x) do { checks++;if(!(x)){printf("FAIL line %d: %s\n",__LINE__,#x);failures++;} } while(0)
static KGame a,b;
static const KLevel simple={"Doors",0,5,6,"#####" "#a* #" "#   #" "##-##" "  .  " "     "};
static const KLevel shape={"Shape",0,7,5,"#######" "#aa   #" "#a*   #" "#   . #" "#######"};
static const KLevel corridor={"Counting",0,8,4,"########" "#aa  *.#" "#      #" "########"};
static unsigned long random_state=1234567;
#include "solution.inc"
static unsigned next(void){random_state=(random_state*1664525UL+1013904223UL)&0xffffffffUL;return (unsigned)(random_state>>16);}
static void boards(void)
{
    int level,x,y,i,j,dx,dy,before[K_CELLS],after[K_CELLS];char copy[K_CELLS],id;unsigned r;
    for(level=0;level<K_LEVELS;level++){
        CHECK(k_begin(&a,k_levels+level));CHECK(!k_won(&a));
        for(y=0;y<a.level->height;y++)for(x=0;x<a.level->width;x++)CHECK(k_tile(&a,x,y)>=0);
        for(i=0;i<100;i++){
            r=next();x=r%a.level->width;y=next()%a.level->height;id=k_at(&a,x,y);
            j=next()%4;dx=j==0?-1:j==1?1:0;dy=j==2?-1:j==3?1:0;
            if(!k_can(&a,id,dx,dy)||k_won(&a))continue;
            memcpy(copy,a.board,sizeof(copy));
            CHECK(k_move(&a,id,dx,dy)==1);
            memset(before,0,sizeof(before));memset(after,0,sizeof(after));
            for(j=0;j<a.level->width*a.level->height;j++){
                before[(unsigned)copy[j]]++;after[(unsigned)a.board[j]]++;
                if(copy[j]=='#')CHECK(a.board[j]=='#');
            }
            for(j=0;j<127;j++)if(!k_static((char)j))CHECK(before[j]==after[j]);
            for(y=0;y<a.level->height;y++)for(x=0;x<a.level->width;x++)CHECK(k_tile(&a,x,y)>=0);
            CHECK(k_undo(&a));CHECK(!memcmp(copy,a.board,a.level->width*a.level->height));CHECK(k_redo(&a));
        }
        while(k_undo(&a)){}
        CHECK(!memcmp(a.board,a.level->map,a.level->width*a.level->height));CHECK(a.score.moves==0);
    }
}
int main(int argc,char **argv)
{
    const char *path=argc>1?argv[1]:"/tmp/klotski-check-save";FILE *f;unsigned long best=0;char board[K_CELLS];int i;
    CHECK(k_begin(&a,&simple));CHECK(!k_can(&a,'z',1,0));CHECK(!k_can(&a,'#',0,1));
    CHECK(!k_can(&a,'a',2,0));CHECK(!k_can(&a,'a',1,1));CHECK(!k_can(&a,'a',-1,0));
    CHECK(k_at(&a,-1,0)=='#');CHECK(k_at(&a,5,0)=='#');
    CHECK(k_move(&a,'*',1,0)==1);CHECK(k_move(&a,'a',0,1)==1);CHECK(k_move(&a,'a',1,0)==1);
    CHECK(!k_can(&a,'a',0,1)); /* ordinary piece cannot cross a door */
    CHECK(k_begin(&a,&simple));
    for(i=0;i<3;i++)CHECK(k_move(&a,'*',0,1)==1);
    CHECK(k_won(&a));CHECK(a.score.moves==1);CHECK(k_move(&a,'*',0,1)==0);
    CHECK(k_undo(&a));CHECK(!k_won(&a));CHECK(k_redo(&a));CHECK(k_won(&a));
    CHECK(k_begin(&a,&shape));CHECK(!k_can(&a,'a',1,0));
    CHECK(k_move(&a,'*',1,0)==1);CHECK(k_move(&a,'a',1,0)==1);
    CHECK(k_at(&a,2,1)=='a'&&k_at(&a,3,1)=='a'&&k_at(&a,2,2)=='a');
    CHECK(k_begin(&a,&corridor));CHECK(k_move(&a,'a',1,0)==1);CHECK(k_move(&a,'a',1,0)==1);
    CHECK(a.score.moves==1);CHECK(k_move(&a,'a',-1,0)==1);CHECK(a.score.moves==1);
    CHECK(k_move(&a,'a',-1,0)==1);CHECK(a.score.moves==0&&a.score.last==0);
    CHECK(k_undo(&a));CHECK(a.score.moves==1);CHECK(k_redo(&a));CHECK(a.score.moves==0);
    CHECK(k_undo(&a));CHECK(k_move(&a,'a',0,1)==1);CHECK(!k_redo(&a)); /* redo branch discarded */
    CHECK(k_undo(&a));memcpy(board,a.board,sizeof(board));
    CHECK(k_save(&a,path,19)==0);CHECK(k_begin(&b,&corridor));CHECK(k_load(&b,path,&best)==1);
    CHECK(best==19);CHECK(k_best(&corridor,path)==19);CHECK(a.cursor==b.cursor&&a.count==b.count);
    CHECK(!memcmp(board,b.board,sizeof(board)));CHECK(k_redo(&b));
    f=fopen(path,"a");CHECK(f!=NULL);if(f){fputs("garbage",f);fclose(f);}
    CHECK(k_load(&a,path,&best)==-1);CHECK(!memcmp(board,a.board,sizeof(board)));
    f=fopen(path,"w");CHECK(f!=NULL);if(f){fputs("KLOTSKI 1 99999999999999999999 0 0 0\n",f);fclose(f);}
    CHECK(k_load(&a,path,&best)==-1);CHECK(!memcmp(board,a.board,sizeof(board)));
    unlink(path);CHECK(k_load(&a,path,&best)==0);CHECK(!memcmp(board,a.board,sizeof(board)));
    /* Grow the journal through multiple reallocations on both C16 targets. */
    CHECK(k_begin(&a,&corridor));
    for(i=0;i<300;i++)CHECK(k_move(&a,'a',i%2?-1:1,0)==1);
    CHECK(a.score.moves==0);
    for(i=0;i<300;i++)CHECK(k_undo(&a));
    CHECK(!memcmp(a.board,corridor.map,corridor.width*corridor.height));
    for(i=0;i<300;i++)CHECK(k_redo(&a));
    CHECK(a.score.moves==0);
    boards();
    /* A separately generated BFS trace solves an actual original puzzle,
     * exercising its door, outside target, and grouped score end to end. */
    CHECK(k_begin(&a,k_levels));
    for(i=0;i<(int)(sizeof(solution)/sizeof(solution[0]));i++){
        CHECK(!k_won(&a));CHECK(k_move(&a,(char)solution[i][0],solution[i][1],solution[i][2])==1);
    }
    CHECK(k_won(&a));CHECK(a.score.moves==SOLUTION_SCORE);
    CHECK(k_save(&a,path,a.score.moves)==0);CHECK(k_best(k_levels,path)==SOLUTION_SCORE);
    CHECK(k_begin(&b,k_levels));CHECK(k_load(&b,path,&best)==1);CHECK(k_won(&b));CHECK(best==SOLUTION_SCORE);
    CHECK(k_undo(&b));CHECK(!k_won(&b));CHECK(k_redo(&b));CHECK(k_won(&b));
    unlink(path);k_free(&a);k_free(&b);
    printf("KLOTSKI_CHECK checks=%d failures=%d\n",checks,failures);return failures?1:0;
}
