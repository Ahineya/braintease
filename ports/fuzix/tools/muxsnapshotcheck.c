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
int main(int argc,char **argv){
    struct snapshot *s;struct stat st;FILE *f;long length;
    char path[512],*saved,*quoted_args[]={"echo","a'b",NULL},command[RESTART_SIZE],long_arg[RESTART_SIZE+10],*too_long[]={long_arg,NULL};
    int original_fd=17,original_pid=123;
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
    assert(!save_session());
    snprintf(path,sizeof(path),"%s/.mux/work",argv[1]);
    assert(!stat(path,&st)&&(st.st_mode&0777)==0600);
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
    /* Moving a sole pane, or hitting the tab ceiling, cannot alter ownership. */
    break_pane();assert(current==1&&panes[5].tab==1);
    current=0;tabs[0].focus=0;
    for(int i=1;i<TABS;i++)tabs[i].used=1;
    break_pane();assert(current==0&&panes[0].tab==0&&nodes[0].parent==1);
    free(saved);
    puts("MUX_SNAPSHOT_OK: live pane identity, split-tree holes, atomic save, permissions, graph validation and quoted recipes");
    return 0;
}
