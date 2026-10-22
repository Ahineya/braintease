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
static void languages(ui_documents *s,const char *directory) {
    static const struct { const char *file; unsigned language; } automatic[] = {
        {"syntax.txt",UI_LANGUAGE_TEXT},{"syntax.c",UI_LANGUAGE_C},
        {"syntax.cpp",UI_LANGUAGE_CPP},{"syntax.rfl",UI_LANGUAGE_RFL},
        {"syntax.rs",UI_LANGUAGE_RUST},{"syntax.java",UI_LANGUAGE_JAVA},
        {"syntax.sh",UI_LANGUAGE_SHELL},{"syntax.tcl",UI_LANGUAGE_TCL},
        {"syntax.awk",UI_LANGUAGE_AWK},{"syntax.py",UI_LANGUAGE_PYTHON},
        {"syntax.pas",UI_LANGUAGE_PASCAL},{"syntax.json",UI_LANGUAGE_JSON},
        {"syntax.ini",UI_LANGUAGE_INI},{"syntax.md",UI_LANGUAGE_MARKDOWN},
        {"syntax.bf",UI_LANGUAGE_BRAINFUCK},{"Makefile",UI_LANGUAGE_MAKEFILE},
        {"syntax.html",UI_LANGUAGE_HTML},{"syntax.css",UI_LANGUAGE_CSS},
        {"syntax.lisp",UI_LANGUAGE_LISP},{"syntax.scm",UI_LANGUAGE_SCHEME},
        {"syntax.asm",UI_LANGUAGE_ASSEMBLY},{"syntax.bas",UI_LANGUAGE_BASIC},
        {"syntax.js",UI_LANGUAGE_JAVASCRIPT},{"syntax.syma",UI_LANGUAGE_SYMA}
    };
    static const char source[]="const answer = 42;\n";
    unsigned i,j,choice;
    unsigned long styles[7],cells[32],generation,revision,cursor,anchor,lexers=0;
    ui_document *document,*other;
    TextBuffer *buffer;
    char path[UI_DOCUMENT_PATH],renamed[UI_DOCUMENT_PATH];
    CHECK(ui_document_language_count()==UI_LANGUAGE_COUNT);
    CHECK(ui_document_language_at(UI_LANGUAGE_COUNT)==NULL);
    for(i=0;i<ui_document_language_count();i++) {
        const struct ui_document_language *language=ui_document_language_at(i);
        CHECK(language&&language->id==i&&language->lexer<=15&&*language->name);
        for(j=0;j<i;j++)CHECK(strcmp(language->name,ui_document_language_at(j)->name));
        lexers|=1UL<<language->lexer;
    }
    CHECK(lexers==65535UL);
    document=ui_documents_new(s);CHECK(document);
    buffer=ui_document_text(document);
    CHECK(ui_document_get_language(document)==UI_LANGUAGE_AUTOMATIC);
    CHECK(ui_document_effective_language(document)==UI_LANGUAGE_TEXT);
    CHECK(!strcmp(ui_document_language_name(document),"Plain text"));
    CHECK(ui_document_set_language(NULL,UI_LANGUAGE_C)==-1);
    for(i=0;i<ui_document_language_count();i++) {
        CHECK(ui_document_set_language(document,i)==0);
        CHECK(ui_document_get_language(document)==i&&!text_modified(buffer));
        CHECK(ui_document_effective_language(document)==(i?i:UI_LANGUAGE_TEXT));
    }
    edit(document,source);ui_document_view(document,8,2);
    revision=text_revision(buffer);generation=text_generation(buffer);
    for(i=0;i<7;i++)styles[i]=(unsigned long)i<<24;
    CHECK(ui_document_set_language(document,UI_LANGUAGE_TEXT)==0);
    CHECK(text_render(buffer,0,0,32,4,styles,0,0,0,cells)==0&&cells[0]=='c');
    CHECK(ui_document_set_language(document,UI_LANGUAGE_JAVASCRIPT)==0);
    CHECK(text_render(buffer,0,0,32,4,styles,0,0,0,cells)==0&&(cells[0]>>24)==2);
    CHECK(text_generation(buffer)!=generation&&text_revision(buffer)==revision&&text_modified(buffer));
    ui_document_get_view(document,&cursor,&anchor);CHECK(cursor==8&&anchor==2);
    CHECK(text_undo(buffer)>=0&&text_length(buffer)==0&&!text_modified(buffer));
    CHECK(ui_document_set_language(document,UI_LANGUAGE_RFL)==0);
    CHECK(text_redo(buffer)>=0&&text_length(buffer)==strlen(source)&&text_modified(buffer));
    snprintf(path,sizeof(path),"%s/override.py",directory);
    CHECK(ui_document_save(s,document,path,0)==0);
    CHECK(ui_document_get_language(document)==UI_LANGUAGE_RFL&&ui_document_effective_language(document)==UI_LANGUAGE_RFL);
    CHECK(!text_modified(buffer));
    CHECK(ui_document_set_language(document,UI_LANGUAGE_AUTOMATIC)==0);
    CHECK(ui_document_effective_language(document)==UI_LANGUAGE_PYTHON&&!strcmp(ui_document_language_name(document),"Python"));
    CHECK(!text_modified(buffer)&&text_undo(buffer)>=0&&text_length(buffer)==0);
    CHECK(text_redo(buffer)>=0&&text_length(buffer)==strlen(source)&&!text_modified(buffer));
    CHECK(ui_document_set_language(document,UI_LANGUAGE_JAVASCRIPT)==0);
    snprintf(renamed,sizeof(renamed),"%s/override.json",directory);
    CHECK(ui_document_save(s,document,renamed,0)==0);
    CHECK(ui_document_effective_language(document)==UI_LANGUAGE_JAVASCRIPT);
    put(renamed,"let external = true;\n");
    CHECK(ui_document_reload(s,document,0)==0);
    CHECK(ui_document_get_language(document)==UI_LANGUAGE_JAVASCRIPT&&!text_modified(ui_document_text(document)));
    buffer=ui_document_text(document);generation=text_generation(buffer);
    choice=ui_document_get_language(document);
    CHECK(ui_document_set_language(document,UI_LANGUAGE_COUNT)==-1);
    CHECK(ui_document_get_language(document)==choice&&text_generation(buffer)==generation);
    other=ui_documents_new(s);CHECK(other);
    CHECK(ui_document_get_language(other)==UI_LANGUAGE_AUTOMATIC&&ui_document_effective_language(other)==UI_LANGUAGE_TEXT);
    CHECK(ui_document_set_language(other,UI_LANGUAGE_RUST)==0);
    CHECK(ui_documents_select(s,document)==0&&ui_document_get_language(document)==UI_LANGUAGE_JAVASCRIPT);
    CHECK(ui_document_get_language(other)==UI_LANGUAGE_RUST);
    CHECK(ui_documents_close(s,other,1)==0&&ui_documents_close(s,document,1)==0);
    for(i=0;i<sizeof(automatic)/sizeof(automatic[0]);i++) {
        snprintf(path,sizeof(path),"%s/%s",directory,automatic[i].file);put(path,source);
        document=ui_documents_open(s,path);CHECK(document);
        CHECK(ui_document_get_language(document)==UI_LANGUAGE_AUTOMATIC);
        CHECK(ui_document_effective_language(document)==automatic[i].language);
        CHECK(!strcmp(ui_document_language_name(document),ui_document_language_at(automatic[i].language)->name));
        CHECK(ui_document_set_language(document,UI_LANGUAGE_TEXT)==0&&!text_modified(ui_document_text(document)));
        CHECK(ui_documents_close(s,document,0)==0);
    }
    document=ui_documents_open(s,renamed);CHECK(document);
    CHECK(ui_document_get_language(document)==UI_LANGUAGE_AUTOMATIC&&ui_document_effective_language(document)==UI_LANGUAGE_JSON);
    CHECK(ui_documents_close(s,document,0)==0);
}
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
    languages(s,argv[1]);
    ui_documents_delete(s);
    puts("DOCUMENT_MODEL passed transactional UTF-8 load, atomic save, mode preservation, same-size external edits, force/copy, failed reload, symlinks, bounds, 30-octet filenames, tabs, per-document syntax choices/automatic detection/history and HOME preferences");
    return 0;
}
