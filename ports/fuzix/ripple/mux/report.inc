/* Shell-owned restart metadata, carried as a bounded private OSC. Never infer
 * shell commands from typed keys, screen text, or truncated process names. */
#define REPORT_LIMIT (2*(DIRECTORY_SIZE+RESTART_SIZE)+16)
static int report_hex(char *out,unsigned capacity,const char *hex){
    unsigned n=0;int a,b;
    if(!strcmp(hex,"-")){out[0]=0;return 1;}
    while(*hex){
        a=*hex++;if(!*hex)return 0;b=*hex++;
        a=a>='0'&&a<='9'?a-'0':a>='a'&&a<='f'?a-'a'+10:-1;
        b=b>='0'&&b<='9'?b-'0':b>='a'&&b<='f'?b-'a'+10:-1;
        if(a<0||b<0||!(a*16+b)||n+1>=capacity)return 0;
        out[n++]=a*16+b;
    }
    out[n]=0;return 1;
}
static void pane_report(struct vt *v,unsigned c,int event){
    static const char prefix[]="777;mux;1;";
    struct pane *p=v->user;
    unsigned start=sizeof(prefix)-1;
    if(event<0){free(p->report);p->report=NULL;p->report_used=0;return;}
    if(event==1){
        if(p->report){
            char directory[DIRECTORY_SIZE],command[RESTART_SIZE],*a,*b,*end;long group;
            p->report[p->report_used-start]=0;
            a=strchr(p->report,';');b=a?strchr(a+1,';'):NULL;
            if(a&&b){
                *a++=0;*b++=0;errno=0;group=strtol(p->report,&end,10);
                if(!errno&&end!=p->report&&!*end&&group>0&&group<=32767&&report_hex(directory,sizeof(directory),a)&&directory[0]=='/'&&report_hex(command,sizeof(command),b)){
                    strcpy(p->auto_directory,directory);strcpy(p->auto_command,command);
                    p->auto_group=group;p->auto_valid=1;
                    program_title(p,command);
                }
            }
        }
        free(p->report);p->report=NULL;p->report_used=0;return;
    }
    if(p->report_used==65535U)return;
    if(p->report_used<start){
        if(c!=(unsigned char)prefix[p->report_used]){p->report_used=65535U;return;}
        if(++p->report_used==start){p->auto_valid=0;p->report=malloc(REPORT_LIMIT);if(!p->report)p->report_used=65535U;}
        return;
    }
    if(c<32||c>126||p->report_used-start+1>=REPORT_LIMIT){free(p->report);p->report=NULL;p->report_used=65535U;return;}
    p->report[p->report_used++-start]=c;
}
