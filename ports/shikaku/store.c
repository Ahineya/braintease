/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include "shikaku.h"
static int flush_file(FILE *f) {
    if(fflush(f))return -1;
#if defined(__STDC_HOSTED__) && __STDC_HOSTED__
    return fsync(fileno(f));
#else
    sync();return 0;
#endif
}
/* Bounded decimal tokens; malformed saves never partly replace a board. */
static int number(FILE *f,unsigned long *v) {
    int ch,count=0;unsigned long n=0;
    do{ch=fgetc(f);}while(ch==' '||ch=='\n'||ch=='\r'||ch=='\t');
    while(ch>='0'&&ch<='9'){
        if(++count>10||n>429496729UL||(n==429496729UL&&ch>'5'))return 0;
        n=n*10+ch-'0';ch=fgetc(f);
    }
    if(!count||(ch!=EOF&&ch!=' '&&ch!='\n'&&ch!='\r'&&ch!='\t'))return 0;
    *v=n;return 1;
}
int s_load(SGame *g,const char *path) {
    FILE *f=fopen(path,"r");SRect *rects=NULL;char magic[16];unsigned long p,signature,count,a[4];int i,j,ch,result=-1;
    if(!f)return errno==ENOENT?0:-1;
    if(!fgets(magic,sizeof(magic),f)||strcmp(magic,"SHIKAKU 1\n")||!number(f,&p)||p!=(unsigned)g->puzzle||
       !number(f,&signature)||signature!=s_game_signature(g)||!number(f,&count)||count>S_CELLS)goto done;
    rects=count?malloc(count*sizeof(SRect)):NULL;if(count&&!rects)goto done;
    for(i=0;i<(int)count;i++){
        for(j=0;j<4;j++)if(!number(f,a+j)||a[j]>S_MAX)goto done;
        rects[i].x=a[0];rects[i].y=a[1];rects[i].w=a[2];rects[i].h=a[3];
    }
    do{ch=fgetc(f);}while(ch==' '||ch=='\n'||ch=='\r'||ch=='\t');
    if(ch!=EOF||ferror(f))goto done;
    result=s_restore(g,rects,(int)count)==1?1:-1;
done:
    free(rects);if(fclose(f))result=-1;return result;
}
int s_save(SGame *g,const char *path) {
    char temp[512];FILE *f;int i,failed=0;SState *s=s_state(g);
    if(snprintf(temp,sizeof(temp),"%s.tmp-%d",path,(int)getpid())>=(int)sizeof(temp)){errno=ENAMETOOLONG;return -1;}
    f=fopen(temp,"w");if(!f)return -1;
    if(fprintf(f,"SHIKAKU 1\n%d %lu %d\n",g->puzzle,s_game_signature(g),s->count)<0)failed=1;
    for(i=0;i<s->count&&!failed;i++){
        SRect r=s->rects[i];if(fprintf(f,"%d %d %d %d\n",r.x,r.y,r.w,r.h)<0)failed=1;
    }
    if(flush_file(f))failed=1;if(fclose(f))failed=1;
    if(!failed&&rename(temp,path)==0){
#if !defined(__STDC_HOSTED__) || !__STDC_HOSTED__
        sync();
#endif
        return 0;
    }
    unlink(temp);return -1;
}
