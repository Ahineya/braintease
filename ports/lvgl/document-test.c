/* SPDX-License-Identifier: MIT */
#define _POSIX_C_SOURCE 200809L
#include "ui-document.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <errno.h>
#ifdef GRAPHITE_DOCUMENT_TEST_TEMP
/* Enforce the guest directory-entry bound in this host-only suite. */
#undef mkstemp
extern int mkstemp(char *pattern);
int graphite_document_mkstemp(char *pattern) {
    const char *base=strrchr(pattern,'/');
    if(strlen(base?base+1:pattern)>30) {errno=ENAMETOOLONG;return -1;}
    return mkstemp(pattern);
}
#endif
#define CHECK(p) do { if (!(p)) { fprintf(stderr, "DOCUMENT_FAIL line %d: %s (%s)\n", __LINE__, #p, ui_documents_error(s)); exit(1); } } while (0)
static void put(const char *p, const char *text) { FILE *f=fopen(p,"wb"); if(!f||fwrite(text,1,strlen(text),f)!=strlen(text)||fclose(f)) abort(); }
static void expect(const char *p,const char *text) { char b[512]; FILE *f=fopen(p,"rb"); size_t n; if(!f)abort();n=fread(b,1,sizeof(b)-1,f);b[n]=0;fclose(f);if(strcmp(b,text))abort(); }
static void edit(ui_document *d,const char *p) { TextBuffer *b=ui_document_text(d);text_begin(b,0);if(text_replace(b,0,text_length(b),(const unsigned char*)p,strlen(p)))abort();text_end(b,strlen(p)); }
int main(int argc,char **argv) {
    ui_documents *s;
    ui_document *a,*b,*c;
    TextBuffer *old;
    char one[2048],two[2048],three[2048],linkpath[2048],settings[2048],longname[2048];
    struct stat st;
    unsigned long cursor,anchor;
    unsigned i;
    FILE *f;
    if(argc!=2)return 2;
    snprintf(one,sizeof(one),"%s/one.txt",argv[1]);
    snprintf(two,sizeof(two),"%s/two.txt",argv[1]);
    snprintf(three,sizeof(three),"%s/new.txt",argv[1]);
    snprintf(linkpath,sizeof(linkpath),"%s/link.txt",argv[1]);
    snprintf(settings,sizeof(settings),"%s/.lvedit",argv[1]);
    snprintf(longname,sizeof(longname),"%s/12345678901234567890123456.txt",argv[1]);
    s=ui_documents_create(argv[1]);CHECK(s);
    CHECK(ui_documents_wrap(s)==1);
    a=ui_documents_new(s);CHECK(a&&ui_documents_count(s)==1);
    CHECK(!text_modified(ui_document_text(a)));
    edit(a,"hello \xd0\x9f\xd1\x80\xd0\xb8\xd0\xb2\xd1\x96\xd1\x82\r\nsecond\n");
    CHECK(text_modified(ui_document_text(a)));
    CHECK(ui_documents_close(s,a,0)==UI_DOCUMENT_DIRTY);
    CHECK(ui_document_save(s,a,one,0)==0);
    CHECK(!text_modified(ui_document_text(a)));
    CHECK(!ui_document_external(s,a));
    CHECK(stat(one,&st)==0 && (st.st_mode&0777)==0600);
    CHECK(ui_documents_open(s,one)==a && ui_documents_count(s)==1);
    ui_document_view(a,5,2);ui_document_get_view(a,&cursor,&anchor);CHECK(cursor==5&&anchor==2);
    put(two,"other");CHECK(chmod(two,0640)==0);
    b=ui_documents_open(s,two);CHECK(b && b!=a);
    edit(b,"edited");CHECK(ui_document_save(s,b,NULL,0)==0);expect(two,"edited");
    CHECK(stat(two,&st)==0 && (st.st_mode&0777)==0640);
    put(two,"external");
    CHECK(ui_document_external(s,b)==1);
    edit(b,"my edit");CHECK(ui_document_save(s,b,NULL,0)==UI_DOCUMENT_CHANGED);expect(two,"external");
    CHECK(text_modified(ui_document_text(b)));
    CHECK(ui_document_save(s,b,three,0)==0);expect(three,"my edit");expect(two,"external");
    CHECK(!ui_document_external(s,b));
    CHECK(ui_document_save(s,b,one,UI_DOCUMENT_REPLACE)==-1);expect(three,"my edit");
    c=ui_documents_new(s);CHECK(c);edit(c,"replacement");
    CHECK(ui_document_save(s,c,two,0)==UI_DOCUMENT_EXISTS);expect(two,"external");
    CHECK(ui_document_save(s,c,two,UI_DOCUMENT_REPLACE)==0);expect(two,"replacement");
    put(two,"replaced!!!");CHECK(ui_document_save(s,c,NULL,0)==UI_DOCUMENT_CHANGED);
    CHECK(ui_document_save(s,c,NULL,UI_DOCUMENT_FORCE)==0);expect(two,"replacement");
    CHECK(unlink(two)==0);CHECK(ui_document_external(s,c)==1);
    CHECK(ui_document_save(s,c,NULL,0)==UI_DOCUMENT_CHANGED);
    CHECK(ui_document_save(s,c,NULL,UI_DOCUMENT_FORCE)==0);expect(two,"replacement");
    CHECK(symlink(two,linkpath)==0);
    CHECK(ui_document_save(s,c,linkpath,UI_DOCUMENT_REPLACE)==-1);expect(two,"replacement");
    CHECK(ui_document_save(s,c,argv[1],UI_DOCUMENT_REPLACE)==-1);
    old=ui_document_text(c);edit(c,"unsaved");
    CHECK(ui_document_reload(s,c,0)==UI_DOCUMENT_DIRTY&&ui_document_text(c)==old);
    CHECK(ui_document_reload(s,c,1)==0&&ui_document_text(c)!=old);
    CHECK(!text_modified(ui_document_text(c)));
    put(two,"nul");f=fopen(two,"ab");CHECK(f);CHECK(fputc(0,f)==0);CHECK(fclose(f)==0);
    old=ui_document_text(c);CHECK(ui_document_reload(s,c,1)==-1&&ui_document_text(c)==old);
    CHECK(!text_modified(old));
    f=fopen(two,"wb");CHECK(f);for(i=0;i<TEXT_MAX+1;i++)CHECK(fputc('x',f)!=EOF);CHECK(fclose(f)==0);
    CHECK(ui_document_reload(s,c,1)==-1&&ui_document_text(c)==old);
    CHECK(ui_documents_open(s,"/nonexistent-lvedit/path")==NULL&&ui_documents_count(s)==3);
    CHECK(ui_document_save(s,a,"/nonexistent-lvedit/path",0)==-1);
    CHECK(!text_modified(ui_document_text(a)));
    ui_documents_set_wrap(s,0);CHECK(ui_documents_settings_save(s)==0);
    CHECK(stat(settings,&st)==0&&(st.st_mode&0777)==0600);
    CHECK(ui_documents_recent_count(s)==3);
    ui_documents_delete(s);s=ui_documents_create(argv[1]);CHECK(s);
    CHECK(!ui_documents_wrap(s)&&ui_documents_recent_count(s)==3);
    CHECK(!strcmp(ui_documents_recent(s,0),two));
    a=ui_documents_new(s);CHECK(a);edit(a,"thirty character basename");
    CHECK(strlen(strrchr(longname,'/')+1)==30);
    CHECK(ui_document_save(s,a,longname,0)==0);expect(longname,"thirty character basename");
    CHECK(!ui_document_external(s,a));
    ui_documents_clear_recent(s);CHECK(ui_documents_settings_save(s)==0);ui_documents_delete(s);
    s=ui_documents_create(argv[1]);CHECK(ui_documents_recent_count(s)==0);
    for(i=0;i<UI_DOCUMENT_TABS;i++)CHECK(ui_documents_new(s));
    CHECK(!ui_documents_new(s)&&ui_documents_count(s)==UI_DOCUMENT_TABS);
    a=ui_documents_at(s,4);CHECK(ui_documents_select(s,a)==0);
    CHECK(ui_documents_close(s,a,1)==0&&ui_documents_count(s)==UI_DOCUMENT_TABS-1);
    CHECK(ui_documents_active(s));
    ui_documents_delete(s);
    /* Malformed and overlong settings do not fabricate recent paths. */
    put(settings,"wrap=banana\nrecent=00\nrecent=2fzz\nrecent=72656c6174697665\nrecent=2f746d702f61\n");
    s=ui_documents_create(argv[1]);CHECK(ui_documents_wrap(s)&&ui_documents_recent_count(s)==1);
    CHECK(!strcmp(ui_documents_recent(s,0),"/tmp/a"));
    ui_documents_delete(s);
    puts("DOCUMENT_MODEL passed transactional UTF-8 load, atomic save, mode preservation, same-size external edits, force/copy, failed reload, symlinks, bounds, 30-octet filenames, tabs and HOME preferences");
    return 0;
}
