/* Exercise the source SDK implementation, including its C16 index widths.
 * USE_INSTALLED_QSORT runs the same contract against a linked guest libc. */
#include <stdio.h>
#include <stdlib.h>
#ifndef USE_INSTALLED_QSORT
#define qsort source_sdk_qsort
#include "../../../ports/fuzix/upstream/Library/libs/qsort.c"
#endif
struct record { unsigned long key, payload; };
static struct record records[9000];
static unsigned long many[32770];
struct big { unsigned long key; unsigned char payload[130]; };
static struct big large[33];
static int nested;
static int compare_long(const void *, const void *);
static int compare_record(const void *a, const void *b) {
    if (!nested) {
        unsigned long inner[3]={2,0,1};nested=1;
        qsort(inner,3,sizeof inner[0],compare_long);
        if(inner[0]!=0 || inner[1]!=1 || inner[2]!=2)nested=-1;
    }
    unsigned long x=((const struct record *)a)->key,y=((const struct record *)b)->key;
    return x<y?-1:x>y;
}
static int compare_long(const void *a, const void *b) {
    unsigned long x=*(const unsigned long *)a,y=*(const unsigned long *)b;
    return x<y?-1:x>y;
}
static int compare_big(const void *a, const void *b) {
    unsigned long x=((const struct big *)a)->key,y=((const struct big *)b)->key;
    return x<y?-1:x>y;
}
static int compare_cell(const void *a, const void *b) {
    unsigned x=*(const unsigned *)a,y=*(const unsigned *)b;return x<y?-1:x>y;
}
int main(void) {
    unsigned long i; unsigned j; int ok=1;
    unsigned narrow[5]={4,1,1,0,2};
    for(i=0;i<9000UL;i++){records[i].key=8999UL-i;records[i].payload=records[i].key^0xabcdeUL;}
    qsort(records,9000UL,sizeof records[0],compare_record);
    for(i=0;i<9000UL;i++)if(records[i].key!=i || records[i].payload!=(i^0xabcdeUL))ok=0;
    if(nested!=1)ok=0;
    putchar(ok?'Y':'N');ok=1;
    for(i=0;i<32770UL;i++)many[i]=32769UL-i;
    qsort(many,32770UL,sizeof many[0],compare_long);
    for(i=0;i<32770UL;i++)if(many[i]!=i)ok=0;
    putchar(ok?'Y':'N');ok=1;
    for(i=0;i<33;i++) {large[i].key=32-i;for(j=0;j<130;j++)large[i].payload[j]=(unsigned)(large[i].key+j);}
    qsort(large,33,sizeof large[0],compare_big);
    for(i=0;i<33;i++){if(large[i].key!=i)ok=0;for(j=0;j<130;j++)if(large[i].payload[j]!=(unsigned)(i+j))ok=0;}
    qsort(narrow,5,sizeof narrow[0],compare_cell);
    qsort(narrow,0,sizeof narrow[0],compare_cell);
    qsort(narrow,1,sizeof narrow[0],compare_cell);
    if(narrow[0]!=0 || narrow[1]!=1 || narrow[2]!=1 || narrow[3]!=2 || narrow[4]!=4)ok=0;
    putchar(ok?'Y':'N');putchar('\n');return 0;
}
