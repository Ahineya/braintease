/* Snapshot and tree invariants without opening PTYs or executing recipes. */
#include <sys/ioctl.h>
#include <assert.h>
#include "../ripple/mux/mux.h"
#include "../ripple/mux/model.inc"
#include "../ripple/mux/snapshot.inc"
static void leaf(int node,int pane,int parent,const char *directory){
    nodes[node].used=1;nodes[node].pane=pane;nodes[node].parent=parent;nodes[node].ratio=50;
    panes[pane].used=1;panes[pane].tab=0;panes[pane].node=node;
    strcpy(panes[pane].directory,directory);
}
static void report(const char *text,int complete){
    struct pane *p=&panes[0];p->vt.user=p;pane_report(&p->vt,0,-1);
    while(*text)pane_report(&p->vt,(unsigned char)*text++,0);
    if(complete)pane_report(&p->vt,0,1);
}
static void reports(void){
    struct pane *p=&panes[0];char huge[REPORT_LIMIT+32];
    report("777;mux;1;123;2f776f726b;277269667427202761206227",1);
    assert(p->auto_valid&&p->auto_group==123&&!strcmp(p->auto_directory,"/work")&&!strcmp(p->auto_command,"'rift' 'a b'"));
    report("0;window title",1);assert(p->auto_valid);
    report("777;mux;1;123;2f;00",1);assert(!p->auto_valid);
    report("777;mux;1;0;2f;-",1);assert(!p->auto_valid);
    report("777;mux;1;123;72656c6174697665;-",1);assert(!p->auto_valid);
    report("777;mux;1;123;2f;-",0);assert(!p->auto_valid);
    pane_report(&p->vt,0,1);assert(p->auto_valid&&!p->auto_command[0]);
    memset(huge,'a',sizeof(huge)-1);memcpy(huge,"777;mux;1;",10);huge[sizeof(huge)-1]=0;
    report(huge,1);assert(!p->auto_valid&&!p->report);
    report("777;mux;1;123;2f;-",1);assert(p->auto_valid);
    pane_report(&p->vt,0,-1);assert(!p->report);
    /* A reported job which no longer owns the PTY cannot supply a recipe. */
    p->fd=-1;assert(!automatic_recipe(p));p->auto_valid=0;
}
int main(int argc,char **argv){
    reports();
    struct snapshot *s;struct stat st;FILE *f;long length;
    char path[512],*saved,*quoted_args[]={"echo","a'b",NULL},command[RESTART_SIZE],long_arg[RESTART_SIZE+10],*too_long[]={long_arg,NULL};
    int original_fd=17,original_pid=123;
    struct stat named,default_stat;char default_path[512];
    assert(argc==2&&!setenv("HOME",argv[1],1));strcpy(session_name,"work");
    leaf(0,0,1,argv[1]);leaf(3,2,4,argv[1]);leaf(5,5,4,argv[1]);
    nodes[1].used=1;nodes[1].pane=-1;nodes[1].parent=-1;nodes[1].a=0;nodes[1].b=4;nodes[1].vertical=1;nodes[1].ratio=55;
    nodes[4].used=1;nodes[4].pane=-1;nodes[4].parent=1;nodes[4].a=3;nodes[4].b=5;nodes[4].ratio=40;
    tabs[0].used=1;tabs[0].root=1;tabs[0].focus=5;tabs[0].zoom=1;strcpy(tabs[0].name,"work");
    panes[5].fd=original_fd;panes[5].pid=original_pid;panes[5].count=88;
    strcpy(panes[5].restart,"printf '%s' \"quoted Ж\"; exec /bin/sh");
    break_pane();
    assert(current==1&&tabs[1].root==5&&panes[5].tab==1&&nodes[5].parent==-1);
    assert(tabs[0].root==1&&nodes[1].b==3&&nodes[3].parent==1&&!nodes[4].used&&!tabs[0].zoom);
    assert(panes[5].fd==original_fd&&panes[5].pid==original_pid&&panes[5].count==88);
    panes[5].restart_override=1;panes[2].directory_override=1;
    assert(!save_session());
    snprintf(default_path,sizeof(default_path),"%s/.mux/default",argv[1]);
    assert(!stat(default_path,&default_stat)&&(default_stat.st_mode&0777)==0600);
    snprintf(path,sizeof(path),"%s/.mux/work",argv[1]);
    assert(!stat(path,&st)&&(st.st_mode&0777)==0600);
    assert(!stat(path,&named)&&named.st_ino!=default_stat.st_ino);
    s=load_snapshot("default");assert(s&&s->panes[5].overrides==1&&s->panes[2].overrides==2&&!s->panes[0].overrides);free(s);
    s=load_session();assert(s&&s->current==1&&s->tabs[1].focus==5&&s->panes[2].used);
    assert(!strcmp(s->panes[5].restart,panes[5].restart)&&s->nodes[1].ratio==55);
    s->nodes[1].a=1;assert(!snapshot_valid(s));free(s);
    f=fopen(path,"r");assert(f);assert(!fseek(f,0,SEEK_END));length=ftell(f);rewind(f);
    saved=calloc(length+16,1);assert(saved&&fread(saved,1,length,f)==(unsigned long)length);fclose(f);
    f=fopen(path,"a");assert(f);fputs("END\n",f);fclose(f);assert(!load_session());
    f=fopen(path,"w");assert(f);fputs(saved,f);fclose(f);
    assert(!chmod(path,0644)&&!load_session());assert(!chmod(path,0600));
    assert(!restart_command(command,quoted_args)&&!strcmp(command,"'echo' 'a'\\''b'"));
    memset(long_arg,'x',sizeof(long_arg)-1);long_arg[sizeof(long_arg)-1]=0;
    assert(restart_command(command,too_long)<0);
    /* Desktop rectangles, titles and stacking order survive the same format. */
    tabs[0].floating=1;tabs[0].focus=2;
    panes[0].fx=2;panes[0].fy=2;panes[0].fw=40;panes[0].fh=12;panes[0].z=0;strcpy(panes[0].title,"Rift");
    panes[2].fx=10;panes[2].fy=4;panes[2].fw=50;panes[2].fh=14;panes[2].z=1;strcpy(panes[2].title,"Music");
    panes[0].minimized=1;panes[0].title_override=1;
    assert(!save_session());s=load_session();assert(s&&s->panes[0].minimized&&s->panes[0].title_override&&s->tabs[0].floating&&s->panes[2].fw==50&&!strcmp(s->panes[0].title,"Rift"));
    s->panes[2].minimized=1;s->tabs[0].zoom=1;assert(!snapshot_valid(s));s->panes[2].minimized=0;s->tabs[0].zoom=0;
    s->panes[0].z=1;assert(!snapshot_valid(s));s->panes[0].z=0;
    s->panes[0].rectangle=0;assert(!snapshot_valid(s));s->panes[0].rectangle=1;
    s->panes[0].fw=VT_MAX_COLS+1;assert(!snapshot_valid(s));free(s);
    /* Version 1 tiled snapshots remain readable. */
    for(int version=1;version<=3;version++){
        saved[12]='0'+version;f=fopen(path,"w");assert(f);
        for(char *at=saved;*at;){char *end=strchr(at,'\n');assert(end);if(strncmp(at,"V ",2)&&(version==3||strncmp(at,"R ",2)))assert(fwrite(at,1,end-at+1,f)==(size_t)(end-at+1));at=end+1;}
        fclose(f);s=load_session();assert(s&&!s->tabs[0].floating&&s->panes[0].overrides==(version==3?0:3));free(s);
    }
    /* Malformed named saves never mutate the independent default. */
    f=fopen(path,"w");assert(f);fputs("bad\n",f);fclose(f);assert(!load_session());
    s=load_snapshot("default");assert(s&&s->tabs[0].floating);free(s);
    /* Moving a sole pane, or hitting the tab ceiling, cannot alter ownership. */
    break_pane();assert(current==1&&panes[5].tab==1);
    current=0;tabs[0].focus=0;
    for(int i=1;i<TABS;i++)tabs[i].used=1;
    break_pane();assert(current==0&&panes[0].tab==0&&nodes[0].parent==1);
    free(saved);
    puts("MUX_SNAPSHOT_OK: live pane identity, split-tree holes, atomic save, permissions, graph validation and quoted recipes");
    return 0;
}
