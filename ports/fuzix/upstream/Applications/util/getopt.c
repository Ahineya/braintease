/*
 * This material, written by Henry Spencer, was released by him
 * into the public domain and is thus not subject to any copyright.
 */
/* Riplix adaptation: guard argv, quote every output argument for Bourne sh,
 * and publish no partial argument vector after an option/allocation error. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <getopt.h>

struct argument { char *text; struct argument *next; };
static int append(struct argument ***tail,const char *s) {
    struct argument *a=malloc(sizeof(*a));
    if (!a) return 0;
    a->text=strdup(s); a->next=NULL;
    if (!a->text) { free(a); return 0; }
    **tail=a; *tail=&a->next; return 1;
}
static void quote(const char *s) {
    putchar('\'');
    while (*s) { if (*s=='\'') fputs("'\\''",stdout); else putchar(*s); s++; }
    putchar('\'');
}
int main(int argc,char **argv) {
    int c,status=0; const char *options; struct argument *head=NULL,**tail=&head,*a;
    if (argc<2) { fprintf(stderr,"usage: getopt OPTSTRING [ARGUMENT ...]\n"); return 2; }
    options=argv[1]; optind=2;
    while ((c=getopt(argc,argv,options))!=-1) {
        char option[3];
        if (c=='?' || c==':') { status=1; continue; }
        option[0]='-'; option[1]=(char)c; option[2]=0;
        if (!append(&tail,option) || (optarg && !append(&tail,optarg))) { status=1; break; }
    }
    if (!status && !append(&tail,"--")) status=1;
    while (!status && optind<argc) if (!append(&tail,argv[optind++])) status=1;
    if (!status) {
        for (a=head;a;a=a->next) { if (a!=head) putchar(' '); quote(a->text); }
        putchar('\n');
    }
    while (head) { a=head->next; free(head->text); free(head); head=a; }
    return status || ferror(stdout);
}
