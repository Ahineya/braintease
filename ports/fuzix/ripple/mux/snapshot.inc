/* Durable restart recipes, not process checkpoints. Parse and validate the
 * complete layout before creating any PTY or executing a saved command. */
struct saved_pane { int used,tab,node;char directory[DIRECTORY_SIZE],restart[RESTART_SIZE]; };
struct snapshot {
    struct options options;
    struct tab tabs[TABS];
    struct node nodes[NODES];
    struct saved_pane panes[PANES];
    int current;
};
static struct snapshot *restoring;

static int snapshot_directory(char *directory,int create){
    const char *home=getenv("HOME");struct stat st;
    if(!home||!*home)home="/";
    if(strlen(home)>300){errno=ENAMETOOLONG;return -1;}
    snprintf(directory,384,"%s/.mux",home);
    if(create&&mkdir(directory,0700)&&errno!=EEXIST)return -1;
    if(lstat(directory,&st))return -1;
    if(!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)){errno=EACCES;return -1;}
    return 0;
}
static int snapshot_name(const char *name){
    unsigned i,n=strlen(name);
    if(!n||n>24)return 0;
    for(i=0;i<n;i++)if(!((name[i]>='a'&&name[i]<='z')||(name[i]>='A'&&name[i]<='Z')||
        (name[i]>='0'&&name[i]<='9')||name[i]=='-'||name[i]=='_'))return 0;
    return 1;
}
static void snapshot_hex(FILE *f,const char *text){
    if(!*text){fputc('-',f);return;}
    while(*text)fprintf(f,"%02x",(unsigned)(unsigned char)*text++);
}
static int save_session(void){
    char directory[384],path[420],temporary[420];
    FILE *f;int fd,ok,i;struct stat st;
    if(snapshot_directory(directory,1))return -1;
    snprintf(path,sizeof(path),"%s/%s",directory,session_name);
    if(!lstat(path,&st)&&(!S_ISREG(st.st_mode)||st.st_uid!=getuid())){errno=EACCES;return -1;}
    snprintf(temporary,sizeof(temporary),"%s/.save-XXXXXX",directory);
    fd=mkstemp(temporary);if(fd<0)return -1;
    f=fdopen(fd,"w");if(!f){close(fd);unlink(temporary);return -1;}
    fputs("MUX-SESSION 1\n",f);
    fprintf(f,"O %u %u ",options.history,options.prefix);snapshot_hex(f,options.shell);fputc('\n',f);
    fprintf(f,"A %d\n",current);
    for(i=0;i<TABS;i++)if(tabs[i].used){
        fprintf(f,"T %d %d %d %d ",i,tabs[i].root,tabs[i].focus,tabs[i].zoom);
        snapshot_hex(f,tabs[i].name);fputc('\n',f);
    }
    for(i=0;i<PANES;i++)if(panes[i].used){
        fprintf(f,"P %d %d %d ",i,panes[i].tab,panes[i].node);
        snapshot_hex(f,panes[i].directory);fputc(' ',f);snapshot_hex(f,panes[i].restart);fputc('\n',f);
    }
    for(i=0;i<NODES;i++)if(nodes[i].used){
        struct node *n=&nodes[i];
        fprintf(f,"N %d %d %d %d %d %d %u\n",i,n->pane,n->pane<0?n->a:-1,n->pane<0?n->b:-1,
            n->parent,n->vertical,n->ratio);
    }
    fputs("END\n",f);ok=!ferror(f)&&!fflush(f);
    if(fclose(f))ok=0;
    if(ok&&!rename(temporary,path)){
        snprintf(message,sizeof(message),"Saved %s; mux restore %s recreates its layout",session_name,session_name);
        dirty=1;return 0;
    }
    unlink(temporary);return -1;
}
static int snapshot_number(const char *s,int low,int high,int *out){
    char *end;long value;
    if(!s||!*s)return 0;
    errno=0;value=strtol(s,&end,10);
    if(errno||*end||value<low||value>high)return 0;
    *out=value;return 1;
}
static int snapshot_digit(unsigned ch){
    if(ch>='0'&&ch<='9')return ch-'0';
    if(ch>='a'&&ch<='f')return ch-'a'+10;
    return -1;
}
static int snapshot_text(char *out,unsigned capacity,const char *hex){
    unsigned i,n=strlen(hex);int a,b;
    if(!strcmp(hex,"-")){out[0]=0;return 1;}
    if((n&1)||n/2>=capacity)return 0;
    for(i=0;i<n;i+=2){
        a=snapshot_digit((unsigned char)hex[i]);b=snapshot_digit((unsigned char)hex[i+1]);
        if(a<0||b<0||!(a*16+b))return 0;
        out[i/2]=a*16+b;
    }
    out[n/2]=0;return 1;
}
static int snapshot_tree(struct snapshot *s,int node,int parent,int tab,unsigned char *seen,unsigned char *panes_seen){
    struct node *n;
    if(node<0||node>=NODES||seen[node]||!s->nodes[node].used)return 0;
    seen[node]=1;n=&s->nodes[node];
    if(n->parent!=parent)return 0;
    if(n->pane>=0){
        struct saved_pane *p=&s->panes[n->pane];
        if(panes_seen[n->pane]||!p->used||p->tab!=tab||p->node!=node||n->a!=-1||n->b!=-1)return 0;
        panes_seen[n->pane]=1;return 1;
    }
    return snapshot_tree(s,n->a,node,tab,seen,panes_seen)&&snapshot_tree(s,n->b,node,tab,seen,panes_seen);
}
static int snapshot_valid(struct snapshot *s){
    unsigned char seen[NODES]={0},panes_seen[PANES]={0};
    int i;struct stat st;
    if(!s->tabs[s->current].used||s->options.shell[0]!='/')return 0;
    for(i=0;i<TABS;i++)if(s->tabs[i].used){
        int focus=s->tabs[i].focus;
        if(!s->panes[focus].used||s->panes[focus].tab!=i||
            !snapshot_tree(s,s->tabs[i].root,-1,i,seen,panes_seen))return 0;
    }
    for(i=0;i<NODES;i++)if(s->nodes[i].used&&!seen[i])return 0;
    for(i=0;i<PANES;i++)if(s->panes[i].used){
        if(!panes_seen[i]||s->panes[i].directory[0]!='/'||stat(s->panes[i].directory,&st)||!S_ISDIR(st.st_mode))return 0;
    }
    return 1;
}
static struct snapshot *load_session(void){
    char directory[384],path[420],*line=NULL,*fields[10],*word;
    int fd=-1,count,i,index,numbers[8],have_options=0,have_current=0,ended=0,ok=0;
    FILE *f=NULL;struct stat st;struct snapshot *s=NULL;
    if(snapshot_directory(directory,0))return NULL;
    snprintf(path,sizeof(path),"%s/%s",directory,session_name);
    if(lstat(path,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)||st.st_size<0||(unsigned long)st.st_size>65536UL)goto done;
    fd=open(path,O_RDONLY);if(fd<0)goto done;
    f=fdopen(fd,"r");if(!f){close(fd);goto done;}
    s=calloc(1,sizeof(*s));line=malloc(3072);if(!s||!line)goto done;
    if(!fgets(line,3072,f)||strcmp(line,"MUX-SESSION 1\n"))goto done;
    while(fgets(line,3072,f)){
        if(!strchr(line,'\n'))goto done;
        count=0;word=strtok(line," \t\r\n");
        while(word&&count<10){fields[count++]=word;word=strtok(NULL," \t\r\n");}
        if(word||!count)goto done;
        if(!strcmp(fields[0],"END")){if(count!=1)goto done;ended=1;break;}
        if(!strcmp(fields[0],"O")){
            if(count!=4||have_options++||!snapshot_number(fields[1],0,1000,&numbers[0])||
                !snapshot_number(fields[2],1,26,&numbers[1])||!snapshot_text(s->options.shell,sizeof(s->options.shell),fields[3]))goto done;
            s->options.history=numbers[0];s->options.prefix=numbers[1];
        }else if(!strcmp(fields[0],"A")){
            if(count!=2||have_current++||!snapshot_number(fields[1],0,TABS-1,&s->current))goto done;
        }else if(!strcmp(fields[0],"T")){
            if(count!=6||!snapshot_number(fields[1],0,TABS-1,&index)||s->tabs[index].used||
                !snapshot_number(fields[2],0,NODES-1,&s->tabs[index].root)||
                !snapshot_number(fields[3],0,PANES-1,&s->tabs[index].focus)||
                !snapshot_number(fields[4],0,1,&s->tabs[index].zoom)||!snapshot_text(s->tabs[index].name,sizeof(s->tabs[index].name),fields[5]))goto done;
            s->tabs[index].used=1;
        }else if(!strcmp(fields[0],"P")){
            if(count!=6||!snapshot_number(fields[1],0,PANES-1,&index)||s->panes[index].used||
                !snapshot_number(fields[2],0,TABS-1,&s->panes[index].tab)||
                !snapshot_number(fields[3],0,NODES-1,&s->panes[index].node)||
                !snapshot_text(s->panes[index].directory,sizeof(s->panes[index].directory),fields[4])||
                !snapshot_text(s->panes[index].restart,sizeof(s->panes[index].restart),fields[5]))goto done;
            s->panes[index].used=1;
        }else if(!strcmp(fields[0],"N")){
            if(count!=8||!snapshot_number(fields[1],0,NODES-1,&index)||s->nodes[index].used)goto done;
            for(i=0;i<6;i++)if(!snapshot_number(fields[i+2],i==0?-1:i<4?-1:i==4?0:10,
                i==0?PANES-1:i<4?NODES-1:i==4?1:90,&numbers[i]))goto done;
            s->nodes[index].used=1;s->nodes[index].pane=numbers[0];s->nodes[index].a=numbers[1];
            s->nodes[index].b=numbers[2];s->nodes[index].parent=numbers[3];s->nodes[index].vertical=numbers[4];s->nodes[index].ratio=numbers[5];
        }else goto done;
    }
    if(!ended||fgetc(f)!=EOF||ferror(f)||!have_options||!have_current||!snapshot_valid(s))goto done;
    ok=1;
done:
    if(f)fclose(f);free(line);
    if(!ok){free(s);errno=EINVAL;return NULL;}
    return s;
}
static int restore_layout(struct snapshot *s){
    int pmap[PANES],i,p;
    options=s->options;
    for(i=0;i<PANES;i++)if(s->panes[i].used){
        p=create_pane(s->panes[i].tab,NULL,s->panes[i].directory,s->panes[i].restart);
        if(p<0)return -1;pmap[i]=p;
    }
    memcpy(nodes,s->nodes,sizeof(nodes));memcpy(tabs,s->tabs,sizeof(tabs));
    for(i=0;i<NODES;i++)if(nodes[i].used&&nodes[i].pane>=0){
        p=pmap[nodes[i].pane];nodes[i].pane=p;panes[p].node=i;
    }
    for(i=0;i<TABS;i++)if(tabs[i].used)tabs[i].focus=pmap[tabs[i].focus];
    current=s->current;need_layout=full=dirty=1;return 0;
}
