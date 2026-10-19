/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "klotski.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>

int k_flush(int fd)
{
#if __STDC_HOSTED__
    return fsync(fd);
#else
    /* These guests expose a filesystem-wide synchronous flush, rather than
     * fsync. It also flushes the inode/directory updates after rename. */
    (void)fd;sync();return 0;
#endif
}

static unsigned long signature(const KLevel *l)
{
    unsigned long v=5381;const char *s=l->map;
    while(*s)v=(v*33+(unsigned)*s++)&0xffffffffUL;
    return v;
}
int k_save(const KGame *g,const char *path,unsigned long best)
{
    char temporary[512];unsigned i;FILE *f;int failed=0,saved;
    if(!g->level||snprintf(temporary,sizeof(temporary),"%s.tmp-%d",path,(int)getpid())>=(int)sizeof(temporary)){errno=EINVAL;return -1;}
    f=fopen(temporary,"w");if(!f)return -1;
    if(fprintf(f,"KLOTSKI 1 %lu %u %u %lu\n",signature(g->level),g->count,g->cursor,best)<0)failed=1;
    for(i=0;i<g->count&&!failed;i++)if(fprintf(f,"%u %d %d\n",(unsigned)g->steps[i].id,g->steps[i].dx,g->steps[i].dy)<0)failed=1;
    if(fflush(f))failed=1;
    if(!failed&&k_flush(fileno(f)))failed=1;
    saved=errno;if(fclose(f)){failed=1;saved=errno;}
    if(!failed&&rename(temporary,path)){failed=1;saved=errno;}
#if !__STDC_HOSTED__
    if(!failed)k_flush(-1);
#endif
    if(failed){unlink(temporary);errno=saved;return -1;}
    return 0;
}
/* Decimal parsing is bounded before arithmetic, including on C16. */
static int number(FILE *f,unsigned long *out)
{
    int c,n=0;unsigned long v=0;
    do c=fgetc(f);while(c==' '||c=='\n'||c=='\r'||c=='\t');
    while(c>='0'&&c<='9'){
        if(++n>10||v>(0xffffffffUL-(unsigned)(c-'0'))/10)return 0;
        v=v*10+(unsigned)(c-'0');c=fgetc(f);
    }
    if(!n||!(c==EOF||c==' '||c=='\n'||c=='\r'||c=='\t'))return 0;
    *out=v;return 1;
}
static int direction(FILE *f,int *out)
{
    int c,neg=0;unsigned long v;
    do c=fgetc(f);while(c==' '||c=='\n'||c=='\r'||c=='\t');
    if(c=='-')neg=1;else if(c!=EOF)ungetc(c,f);
    if(!number(f,&v)||v>1)return 0;*out=neg?-(int)v:(int)v;return 1;
}
unsigned long k_best(const KLevel *l,const char *path)
{
    FILE *f;char magic[8];unsigned long v,s,n,c,b=0;
    f=fopen(path,"r");if(!f)return 0;
    if(fread(magic,1,8,f)!=8||memcmp(magic,"KLOTSKI ",8)||!number(f,&v)||v!=1||
       !number(f,&s)||s!=signature(l)||!number(f,&n)||n>K_HISTORY||
       !number(f,&c)||c>n||!number(f,&b)||b>K_HISTORY)b=0;
    fclose(f);return b;
}
int k_load(KGame *g,const char *path,unsigned long *best)
{
    FILE *f;KGame next;unsigned long sig,count,cursor,b,id,version;unsigned i;int dx,dy,c,ok=0;char magic[8];
    if(!g->level){errno=EINVAL;return -1;}
    f=fopen(path,"r");if(!f)return errno==ENOENT?0:-1;
    memset(&next,0,sizeof(next));
    if(fread(magic,1,8,f)!=8||memcmp(magic,"KLOTSKI ",8)||!number(f,&version)||version!=1||
       !number(f,&sig)||sig!=signature(g->level)||!number(f,&count)||count>K_HISTORY||
       !number(f,&cursor)||cursor>count||!number(f,&b)||b>K_HISTORY)goto done;
    if(!k_begin(&next,g->level))goto done;
    for(i=0;i<(unsigned)count;i++){
        if(!number(f,&id)||id>126||!direction(f,&dx)||!direction(f,&dy)||k_move(&next,(char)id,dx,dy)!=1)goto done;
    }
    do c=fgetc(f);while(c==' '||c=='\n'||c=='\r'||c=='\t');
    if(c!=EOF||ferror(f))goto done;
    while(next.cursor>(unsigned)cursor)k_undo(&next);
    ok=1;
done:
    if(fclose(f))ok=0;
    if(ok){k_free(g);*g=next;*best=b;return 1;}
    k_free(&next);errno=EINVAL;return -1;
}
