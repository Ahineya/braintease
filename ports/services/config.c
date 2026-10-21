/* SPDX-License-Identifier: MIT */
#include "service.h"
#include "ini.h"
int sv_name(const char *s){
    unsigned i,n=strlen(s);if(!n||n>=SV_NAME)return 0;
    for(i=0;i<n;i++)if(!((s[i]>='a'&&s[i]<='z')||(s[i]>='A'&&s[i]<='Z')||(s[i]>='0'&&s[i]<='9')||s[i]=='-'||s[i]=='_'))return 0;
    return 1;
}
size_t sv_find(struct services *all,const char *name){size_t i;for(i=0;i<all->count;i++)if(!strcmp(all->item[i].name,name))return i;return SV_NONE;}
/* Allocation bounds follow the target's address space, not a service quota. */
static void *reserve(void *items,size_t *capacity,size_t needed,size_t width){
    size_t n,maximum=((size_t)-1)/width;void *grown;
    if(needed<=*capacity)return items;
    if(needed>maximum){errno=ENOMEM;return NULL;}
    n=*capacity?*capacity:4;
    if(n>maximum)n=maximum;
    while(n<needed){if(n>maximum/2){n=maximum;break;}n*=2;}
    grown=realloc(items,n*width);if(!grown)return NULL;
    *capacity=n;return grown;
}
int sv_has(const struct sv_links *links,size_t index){size_t i;for(i=0;i<links->count;i++)if(links->item[i]==index)return 1;return 0;}
static int link_add(struct sv_links *links,size_t index){
    size_t *grown;
    if(sv_has(links,index))return 0;
    if(links->count==(size_t)-1)return -1;
    grown=reserve(links->item,&links->capacity,links->count+1,sizeof(*links->item));
    if(!grown)return -1;
    links->item=grown;links->item[links->count++]=index;return 0;
}
void sv_free(struct services *all){
    size_t i;for(i=0;i<all->count;i++){struct service *s=&all->item[i];free(s->after);free(s->requires);free(s->before.item);free(s->needs.item);}
    free(all->item);free(all->order);free(all->marks);memset(all,0,sizeof(*all));
}
static int copy_names(char **out,const char *text){
    *out=malloc(strlen(text)+1);if(!*out)return -1;strcpy(*out,text);return 0;
}
char *sv_read_config(const char *path,size_t *length,char *error,unsigned capacity){
    struct stat st;FILE *file;char *text;
    if(lstat(path,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&022)||st.st_size<0||st.st_size>SV_DEFINITIONS){snprintf(error,capacity,"unsafe, missing or oversized configuration: %s",path);return NULL;}
    file=fopen(path,"r");if(!file){snprintf(error,capacity,"cannot read %s",path);return NULL;}
    text=malloc(SV_DEFINITIONS+1);if(!text){fclose(file);snprintf(error,capacity,"out of memory");return NULL;}
    *length=fread(text,1,SV_DEFINITIONS+1,file);
    if(ferror(file)||*length>SV_DEFINITIONS){free(text);text=NULL;snprintf(error,capacity,"cannot read bounded configuration: %s",path);}
    else text[*length]=0;
    fclose(file);return text;
}
static int number(const char *text,unsigned low,unsigned high,unsigned *out){
    char *end;unsigned long n;errno=0;n=strtoul(text,&end,10);
    if(errno||!*text||*end||*text=='-'||n<low||n>high)return 0;*out=n;return 1;
}
static int definition(struct service *s,const char *path,char *error,unsigned capacity){
    size_t length;char *text=sv_read_config(path,&length,error,capacity);IniReader *reader;int next=0,ok=0;unsigned bit,index;const char *key,*value;
    if(!text)return -1;
    reader=ini_reader_with_limits(text,length,SV_DEFINITIONS,SV_DEFINITIONS,64);free(text);
    if(!reader){snprintf(error,capacity,"out of memory");return -1;}
    strcpy(s->directory,"/");s->start_ms=10000;s->stop_ms=2000;s->restart_ms=1000;s->restart_limit=5;s->exec_fd=s->ready_fd=-1;
    while((next=ini_next(reader))>0){
        key=ini_key(reader);value=ini_value(reader);
        if(!strcmp(ini_section(reader),"arguments")){
            if(!number(key,1,SV_ARGS,&index)||(s->arg_seen&(1U<<(index-1)))||strlen(value)>=sizeof(s->args[0]))goto done;
            strcpy(s->args[index-1],value);s->arg_seen|=1U<<(index-1);if(index>s->argc)s->argc=index;continue;
        }
        if(strcmp(ini_section(reader),"service"))goto done;
        if(!strcmp(key,"program")){bit=1;if(value[0]!='/'||strlen(value)>=sizeof(s->program))goto done;strcpy(s->program,value);}
        else if(!strcmp(key,"directory")){bit=2;if(value[0]!='/'||strlen(value)>=sizeof(s->directory))goto done;strcpy(s->directory,value);}
        else if(!strcmp(key,"type")){bit=4;if(strcmp(value,"simple")&&strcmp(value,"oneshot"))goto done;s->oneshot=!strcmp(value,"oneshot");}
        else if(!strcmp(key,"ready")){bit=8;if(strcmp(value,"exec")&&strcmp(value,"notify"))goto done;s->notify=!strcmp(value,"notify");}
        else if(!strcmp(key,"restart")){bit=16;if(!strcmp(value,"no"))s->restart=SV_NO;else if(!strcmp(value,"on-failure"))s->restart=SV_FAILURE;else if(!strcmp(value,"always"))s->restart=SV_ALWAYS;else goto done;}
        else if(!strcmp(key,"restart_limit")){bit=32;if(!number(value,0,100,&s->restart_limit))goto done;}
        else if(!strcmp(key,"start_timeout_ms")){bit=64;if(!number(value,100,60000,&s->start_ms))goto done;}
        else if(!strcmp(key,"stop_timeout_ms")){bit=128;if(!number(value,100,60000,&s->stop_ms))goto done;}
        else if(!strcmp(key,"restart_delay_ms")){bit=256;if(!number(value,100,60000,&s->restart_ms))goto done;}
        else if(!strcmp(key,"after")){bit=512;if(!(s->seen&bit)&&copy_names(&s->after,value))goto memory;}
        else if(!strcmp(key,"requires")){bit=1024;if(!(s->seen&bit)&&copy_names(&s->requires,value))goto memory;}
        else goto done;
        if(s->seen&bit)goto done;s->seen|=bit;
    }
    if(next<0||!(s->seen&1)||s->arg_seen!=(1U<<s->argc)-1||(s->oneshot&&(s->notify||s->restart==SV_ALWAYS)))goto done;
    ok=1;
done:
    if(!ok)snprintf(error,capacity,"%s: invalid/duplicate setting or argument gap at line %lu",path,ini_line(reader));
    ini_free(reader);return ok?0:-1;
memory:
    ini_free(reader);snprintf(error,capacity,"out of memory reading %s",path);return -1;
}
/* Tokenize an owned copy; adjacency lists contain only actual edges. */
static int names(struct services *all,char *text,struct sv_links *links){
    char *word;size_t index;if(!text)return 0;
    word=strtok(text," ,\t");
    while(word){if(!sv_name(word)||(index=sv_find(all,word))==SV_NONE)return -1;
        if(link_add(links,index))return -2;word=strtok(NULL," ,\t");}return 0;
}
int sv_load(struct services *all,const char *directory,char *error,unsigned capacity){
    DIR *dir;struct dirent *entry;struct stat st;size_t i,j,n,position=0;int result;char path[512],name[SV_NAME];
    memset(all,0,sizeof(*all));
    if(lstat(directory,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&022)){snprintf(error,capacity,"unsafe configuration directory");return -1;}
    dir=opendir(directory);if(!dir){snprintf(error,capacity,"cannot open configuration directory");return -1;}
    while((entry=readdir(dir))){
        struct service *s,*grown;
        n=strlen(entry->d_name);if(n<=4||strcmp(entry->d_name+n-4,".ini"))continue;
        if(n-4>=SV_NAME)goto bad_directory;
        memcpy(name,entry->d_name,n-4);name[n-4]=0;if(!sv_name(name))goto bad_directory;
        if(all->count==(size_t)-1){closedir(dir);goto memory;}
        grown=reserve(all->item,&all->capacity,all->count+1,sizeof(*all->item));
        if(!grown){closedir(dir);goto memory;}
        all->item=grown;s=&all->item[all->count++];memset(s,0,sizeof(*s));strcpy(s->name,name);
    }
    closedir(dir);
    if(!all->count)return 0;
    if(all->count>(size_t)-1/sizeof(*all->order))goto memory;
    all->order=malloc(all->count*sizeof(*all->order));all->marks=calloc(all->count,1);
    if(!all->order||!all->marks)goto memory;
    /* Stable planning independent of filesystem directory order. */
    for(i=0;i<all->count;i++)for(j=i+1;j<all->count;j++)if(strcmp(all->item[i].name,all->item[j].name)>0){strcpy(name,all->item[i].name);strcpy(all->item[i].name,all->item[j].name);strcpy(all->item[j].name,name);}
    for(i=0;i<all->count;i++){
        if(strlen(directory)+strlen(all->item[i].name)+6>=sizeof(path)){snprintf(error,capacity,"configuration path too long");return -1;}
        snprintf(path,sizeof(path),"%s/%s.ini",directory,all->item[i].name);if(definition(&all->item[i],path,error,capacity))return -1;
    }
    for(i=0;i<all->count;i++){
        struct service *s=&all->item[i];
        result=names(all,s->requires,&s->needs);if(!result)result=names(all,s->after,&s->before);
        if(result==-2)goto memory;
        if(result){snprintf(error,capacity,"%s: unknown dependency",s->name);return -1;}
        for(j=0;j<s->needs.count;j++)if(link_add(&s->before,s->needs.item[j]))goto memory;
        free(s->after);free(s->requires);s->after=s->requires=NULL;
    }
    while(position<all->count){
        for(i=0;i<all->count;i++)if(!all->marks[i]){
            struct sv_links *before=&all->item[i].before;
            for(j=0;j<before->count&&all->marks[before->item[j]];j++);
            if(j==before->count)break;
        }
        if(i==all->count){snprintf(error,capacity,"dependency cycle");return -1;}
        all->order[position++]=i;all->marks[i]=1;
    }
    return 0;
bad_directory:
    closedir(dir);snprintf(error,capacity,"invalid service definition name");return -1;
memory:
    snprintf(error,capacity,"out of memory loading service definitions");return -1;
}
/* The validated topological order lets closure traversal avoid recursive stacks. */
void sv_select(struct services *all,size_t index){
    size_t i,j;memset(all->marks,0,all->count);all->marks[index]=1;
    for(i=all->count;i>0;i--){size_t at=all->order[i-1];
        if(all->marks[at]){struct sv_links *needs=&all->item[at].needs;
            for(j=0;j<needs->count;j++)all->marks[needs->item[j]]=1;}
    }
}
void sv_require(struct services *all,size_t index){
    size_t i;sv_select(all,index);for(i=0;i<all->count;i++)if(all->marks[i])all->item[i].desired=1;
}
int sv_profile(struct services *all,const char *path,char *error,unsigned capacity){
    size_t length,i;char *text,*value;IniReader *r;int next,seen=0,ok=0,result=0;struct sv_links enabled={NULL,0,0};
    if(!path)return 0;
    text=sv_read_config(path,&length,error,capacity);if(!text)return -1;
    r=ini_reader_with_limits(text,length,SV_DEFINITIONS,SV_DEFINITIONS,2);free(text);if(!r){snprintf(error,capacity,"out of memory");return -1;}
    while((next=ini_next(r))>0){
        if(strcmp(ini_section(r),"services")||strcmp(ini_key(r),"enabled")||seen++)goto done;
        value=NULL;if(copy_names(&value,ini_value(r))){result=-2;goto done;}
        result=names(all,value,&enabled);free(value);if(result)goto done;
    }
    if(next<0)goto done;
    for(i=0;i<enabled.count;i++)sv_require(all,enabled.item[i]);ok=1;
done:
    free(enabled.item);ini_free(r);
    if(!ok)snprintf(error,capacity,result==-2?"out of memory reading profile":"invalid profile or unknown enabled service");return ok?0:-1;
}
int sv_runtime(const char *directory,int create,char *socket_path,unsigned capacity){
    struct stat st;
    if(strlen(directory)+14>=capacity){errno=ENAMETOOLONG;return -1;}
    if(create&&mkdir(directory,0700)&&errno!=EEXIST)return -1;
    if(lstat(directory,&st)||!S_ISDIR(st.st_mode)||st.st_uid!=getuid()||(st.st_mode&077)){errno=EACCES;return -1;}
    snprintf(socket_path,capacity,"%s/control.sock",directory);return 0;
}
