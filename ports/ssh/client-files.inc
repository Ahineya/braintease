/* SPDX-License-Identifier: GPL-3.0-or-later */
static int remote_name(char *spec,char *endpoint,unsigned cap,char **path) {
    char *colon=strchr(spec,':'),*slash=strchr(spec,'/');unsigned n;
    if(!colon||(slash&&slash<colon))return 0;
    n=colon-spec;if(!n||n>=cap||strchr(colon+1,':'))return -1;
    memcpy(endpoint,spec,n);endpoint[n]=0;*path=colon+1;return 1;
}
static const char *base_name(const char *path) {
    const char *p=strrchr(path,'/');p=p?p+1:path;
    return !*p||!strcmp(p,".")||!strcmp(p,"..")?NULL:p;
}
static int path_join(char *out,unsigned cap,const char *base,const char *path) {
    int n;if(!*path)path=".";
    n=*path=='/'?snprintf(out,cap,"%s",path):snprintf(out,cap,"%s/%s",base,path);
    if(n<0||(unsigned)n>=cap){errno=ENAMETOOLONG;return -1;}return 0;
}
static int remote_stat(struct connection *c,char *path,WS_SFTP_FILEATRB *a) {
    unsigned long start=now();int32_t r;int again;
    do{r=wolfSSH_SFTP_STAT(c->ssh,path,a);again=retry(c,r,start);}while(again>0);return again<0?-1:0;
}
static int remote_close(struct connection *c,byte *handle,word32 length) {
    unsigned long start=now();int32_t r;int again;
    do{r=wolfSSH_SFTP_Close(c->ssh,handle,length);again=retry(c,r,start);}while(again>0);return again<0?-1:0;
}
static WS_SFTPNAME *remote_names(struct connection *c,char *path,int list) {
    unsigned long start=now();WS_SFTPNAME *names;int e;
    for(;;) {
        names=list?wolfSSH_SFTP_LS(c->ssh,path):wolfSSH_SFTP_RealPath(c->ssh,path);
        if(names)return names;e=wolfSSH_get_error(c->ssh);
        if(!transient(e)){fprintf(stderr,"sftp: cannot read %s (%s)\n",path,wolfSSH_ErrorToName(e));return NULL;}
        if(wait_socket(c,start,120000UL))return NULL;
    }
}
/* Guest directories still have NAME_MAX-sized on-disk components. Reject
 * unrepresentable paths before lookup: the kernel must never truncate a name. */
static int local_path(const char *path) {
    char part[SSH_PATH];unsigned i,n=0,component=0;struct stat st;
    for(i=0;;i++) {
        if(i>=sizeof(part)-1){errno=ENAMETOOLONG;return -1;}
        part[n++]=path[i];
        if(path[i]=='/'||!path[i]) {
            if(component>NAME_MAX){errno=ENAMETOOLONG;return -1;}component=0;
            part[n-1]=0;
            if(n>1&&lstat(part,&st)==0&&S_ISLNK(st.st_mode)){errno=ELOOP;return -1;}
            part[n-1]=path[i];if(!path[i])break;
        } else component++;
    }
    return 0;
}
static int remote_lstat(struct connection *c,char *path,WS_SFTP_FILEATRB *a) {
    unsigned long start=now();int32_t r;int again;
    do{r=wolfSSH_SFTP_LSTAT(c->ssh,path,a);again=retry(c,r,start);}while(again>0);return again<0?-1:0;
}
static int transfer(struct connection *c,char *remote,const char *local,int upload) {
    byte handle[WOLFSSH_MAX_HANDLE],data[8192];word32 handlelen=sizeof(handle),offset[2]={0,0};
    WS_SFTP_FILEATRB attrs;struct stat st;int fd=-1,result=-1,opened=0,again,n,i,saved;int32_t r;
    unsigned at,length;unsigned long start;char temporary[SSH_PATH];
    memset(&attrs,0,sizeof(attrs));temporary[0]=0;
    if(local_path(local))goto done;
    if(upload) {
        fd=open(local,O_RDONLY);if(fd<0)goto done;
        if(fstat(fd,&st)<0||!S_ISREG(st.st_mode)||st.st_size<0){errno=EINVAL;goto done;}
        attrs.flags=WOLFSSH_FILEATRB_PERM;attrs.per=0600;
    } else {
        /* Short sibling names also permit a full NAME_MAX destination name. */
        const char *slash=strrchr(local,'/');unsigned parent=slash?(unsigned)(slash-local+1):0,attempt;
        if(lstat(local,&st)==0&&!S_ISREG(st.st_mode)){errno=EINVAL;goto done;}
        if(parent+40>=sizeof(temporary)){errno=ENAMETOOLONG;goto done;}
        memcpy(temporary,local,parent);
        for(attempt=0;attempt<100;attempt++) {
            snprintf(temporary+parent,sizeof(temporary)-parent,".scp-%u-%u",(unsigned)getpid(),attempt);
            fd=open(temporary,O_WRONLY|O_CREAT|O_EXCL,0600);if(fd>=0)break;
            if(errno!=EEXIST)break;
        }
        if(fd<0){temporary[0]=0;goto done;}
    }
    start=now();
    do {
        r=wolfSSH_SFTP_Open(c->ssh,remote,upload?(WOLFSSH_FXF_WRITE|WOLFSSH_FXF_CREAT|WOLFSSH_FXF_TRUNC):WOLFSSH_FXF_READ,&attrs,handle,&handlelen);
        again=retry(c,r,start);
    }while(again>0);
    if(again<0)goto done;opened=1;
    while(!stopping) {
        if(upload) {
            n=read(fd,data,sizeof(data));if(n<0&&errno==EINTR)continue;if(n<0)goto done;if(!n)break;length=n;
            for(i=0;i<n;i++)if(data[i]>255){errno=EINVAL;goto done;}
            at=0;
            while(at<length) {
                start=now();do{r=wolfSSH_SFTP_SendWritePacket(c->ssh,handle,handlelen,offset,data+at,length-at);again=retry(c,r,start);}while(again>0);
                if(again<0||r<=0||r>(int32_t)(length-at)){errno=EIO;goto done;}at+=r;offset[0]+=r;
            }
        } else {
            start=now();do{r=wolfSSH_SFTP_SendReadPacket(c->ssh,handle,handlelen,offset,data,sizeof(data));again=retry(c,r,start);}while(again>0);
            if(again<0||r>(int32_t)sizeof(data))goto done;if(!r)break;length=r;at=0;
            for(i=0;i<(int)length;i++)if(data[i]>255){errno=EINVAL;goto done;}
            while(at<length){n=write(fd,data+at,length-at);if(n<0&&errno==EINTR)continue;if(n<=0)goto done;at+=n;}
            offset[0]+=length;
        }
        /* Guest file offsets are signed 32-bit. Reject overflow explicitly. */
        if(offset[0]>2147475455UL){errno=EFBIG;goto done;}
    }
    if(stopping){errno=EINTR;goto done;}
    if(remote_close(c,handle,handlelen)){opened=0;goto done;}opened=0;
    if(close(fd)<0){fd=-1;goto done;}fd=-1;
    if(!upload&&rename(temporary,local)<0)goto done;
    temporary[0]=0;result=0;
done:
    saved=errno;if(opened)remote_close(c,handle,handlelen);if(fd>=0)close(fd);if(temporary[0])unlink(temporary);
    if(result){errno=saved?saved:EIO;perror(upload?"sftp: upload":"sftp: download");}
    return result;
}
static int remote_file_target(struct connection *c,char *path) {
    WS_SFTP_FILEATRB attrs;unsigned long start=now();int32_t r;int again;
    do {
        r=wolfSSH_SFTP_LSTAT(c->ssh,path,&attrs);
        if(r<0&&wolfSSH_get_error(c->ssh)==WS_SFTP_FILE_DNE)return 0;
        again=retry(c,r,start);
    }while(again>0);
    if(again<0)return -1;
    if(!(attrs.flags&WOLFSSH_FILEATRB_PERM)||(attrs.per&0170000)!=0100000){errno=EINVAL;return -1;}
    return 0;
}
static int remote_tree_path(struct connection *c,const char *path) {
    char part[SSH_PATH];unsigned i,n=strlen(path);WS_SFTP_FILEATRB attrs;int32_t r;int again;unsigned long start;
    if(n>=sizeof(part)){errno=ENAMETOOLONG;return -1;}memcpy(part,path,n+1);
    for(i=1;i<=n;i++)if(part[i]=='/'||!part[i]) {
        char saved=part[i];part[i]=0;start=now();
        do {
            r=wolfSSH_SFTP_LSTAT(c->ssh,part,&attrs);
            if(i==n&&r<0&&wolfSSH_get_error(c->ssh)==WS_SFTP_FILE_DNE){part[i]=saved;return 0;}
            again=retry(c,r,start);
        }while(again>0);
        part[i]=saved;if(again<0)return -1;
        if(!(attrs.flags&WOLFSSH_FILEATRB_PERM)||(attrs.per&0170000)==0120000){errno=ELOOP;return -1;}
    }
    return 0;
}
/* Directory-tree copies intentionally refuse symlinks and special files.
 * Bounds and component validation apply before constructing descendant paths. */
static int copy_tree(struct connection *c,char *remote,const char *local,int upload,unsigned depth) {
    struct stat st;WS_SFTP_FILEATRB attrs;WS_SFTPNAME *names=NULL,*entry;
    DIR *directory=NULL;struct dirent *de;char *paths=NULL,*rpath,*lpath;unsigned mode;
    int result=-1,again,saved;int32_t r;unsigned long start;
    if(stopping){errno=EINTR;return -1;}
    if(depth>=32){errno=ELOOP;return -1;}
    if(local_path(local))return -1;
    if(upload) {
        if(lstat(local,&st)<0)return -1;
        if(S_ISREG(st.st_mode)){if(remote_file_target(c,remote))return -1;return transfer(c,remote,local,1);}
        if(!S_ISDIR(st.st_mode)){errno=EINVAL;return -1;}
        memset(&attrs,0,sizeof(attrs));attrs.flags=WOLFSSH_FILEATRB_PERM;attrs.per=0700;
        start=now();do{r=wolfSSH_SFTP_MKDIR(c->ssh,remote,&attrs);
            if(r<0&&!transient((int)r)&&!transient(wolfSSH_get_error(c->ssh)))break;
            again=retry(c,r,start);
        }while(again>0);
        if(r<0&&(remote_lstat(c,remote,&attrs)||!(attrs.flags&WOLFSSH_FILEATRB_PERM)||(attrs.per&0170000)!=0040000)){errno=ENOTDIR;return -1;}
        directory=opendir(local);if(!directory)return -1;
    } else {
        if(remote_lstat(c,remote,&attrs)||!(attrs.flags&WOLFSSH_FILEATRB_PERM)){errno=EIO;return -1;}
        mode=attrs.per&0170000;
        if(mode==0100000)return transfer(c,remote,local,0);
        if(mode!=0040000){errno=EINVAL;return -1;}
        if(lstat(local,&st)<0){if(errno!=ENOENT||mkdir(local,0700)<0)return -1;}
        else if(!S_ISDIR(st.st_mode)){errno=ENOTDIR;return -1;}
        names=remote_names(c,remote,1);if(!names)return -1;
        /* Validate the complete response before writing any children. */
        for(entry=names;entry;entry=entry->next) {
            unsigned j;if(!entry->fSz||entry->fSz>NAME_MAX||strlen(entry->fName)!=entry->fSz){errno=ENAMETOOLONG;goto done;}
            for(j=0;j<entry->fSz;j++)if(entry->fName[j]=='/'||(unsigned)entry->fName[j]>255){errno=EINVAL;goto done;}
        }
    }
    paths=malloc(SSH_PATH*2);if(!paths)goto done;rpath=paths;lpath=paths+SSH_PATH;
    entry=names;
    for(;;) {
        const char *name;
        if(stopping){errno=EINTR;goto done;}
        if(upload){errno=0;de=readdir(directory);if(!de){if(errno)goto done;break;}name=de->d_name;}
        else {if(!entry)break;name=entry->fName;}
        if(strcmp(name,".")&&strcmp(name,"..")) {
            if(path_join(rpath,SSH_PATH,remote,name)||path_join(lpath,SSH_PATH,local,name)||copy_tree(c,rpath,lpath,upload,depth+1))goto done;
        }
        if(!upload)entry=entry->next;
    }
    result=0;
done:
    saved=errno;if(directory&&closedir(directory)<0&&!result){saved=errno;result=-1;}
    if(names)wolfSSH_SFTPNAME_list_free(names);free(paths);errno=saved;return result;
}
static void print_name(const char *name) {
    const unsigned char *p=(const unsigned char *)name;
    while(*p){if(*p<32||*p==127||*p>255)printf("\\x%02x",(unsigned)*p&255);else putchar(*p);p++;}
}
static int sftp_command(struct connection *c,char *cwd,char **v,int n) {
    char a[SSH_PATH],b[SSH_PATH],local[SSH_PATH];const char *dest;WS_SFTPNAME *names,*p;WS_SFTP_FILEATRB attrs;
    unsigned long start;int32_t r;int again;struct stat st;
    if(!n)return 0;
    if(!strcmp(v[0],"help")||!strcmp(v[0],"?")) {
        puts("pwd | lpwd | ls [path] | cd path | lcd path\nget remote [local] | put local [remote]\nmkdir path | rmdir path | rm path | rename old new | chmod mode path\nquit (paths may be quoted)");return 0;
    }
    if(!strcmp(v[0],"pwd")&&n==1){puts(cwd);return 0;}
    if(!strcmp(v[0],"lpwd")&&n==1){if(!getcwd(local,sizeof(local)))return -1;puts(local);return 0;}
    if(!strcmp(v[0],"lcd")&&n==2)return chdir(v[1]);
    if(!strcmp(v[0],"ls")&&n<=2) {
        if(path_join(a,sizeof(a),cwd,n==2?v[1]:"."))return -1;
        names=remote_names(c,a,1);if(!names)return -1;
        for(p=names;p;p=p->next){print_name(p->fName);if((p->atrb.flags&WOLFSSH_FILEATRB_PERM)&&(p->atrb.per&0170000)==0040000)putchar('/');putchar('\n');}
        wolfSSH_SFTPNAME_list_free(names);return 0;
    }
    if(!strcmp(v[0],"cd")&&n==2) {
        if(path_join(a,sizeof(a),cwd,v[1])||remote_stat(c,a,&attrs))return -1;
        if(!(attrs.flags&WOLFSSH_FILEATRB_PERM)||(attrs.per&0170000)!=0040000){errno=ENOTDIR;return -1;}
        names=remote_names(c,a,0);if(!names)return -1;r=copy_text(cwd,SSH_PATH,names->fName);wolfSSH_SFTPNAME_list_free(names);return r;
    }
    if((!strcmp(v[0],"get")||!strcmp(v[0],"put"))&&(n==2||n==3)) {
        int upload=v[0][0]=='p';dest=n==3?v[2]:base_name(v[1]);if(!dest){errno=EINVAL;return -1;}
        if(path_join(a,sizeof(a),cwd,upload?dest:v[1]))return -1;
        if(!upload&&stat(dest,&st)==0&&S_ISDIR(st.st_mode)){
            const char *base=base_name(v[1]);if(!base||path_join(local,sizeof(local),dest,base))return -1;dest=local;
        }
        return transfer(c,a,upload?v[1]:dest,upload);
    }
    if(n!=2&&n!=3)goto usage;
    if(path_join(a,sizeof(a),cwd,!strcmp(v[0],"chmod")&&n==3?v[2]:v[1]))return -1;
    if(!strcmp(v[0],"rename")&&n==3&&path_join(b,sizeof(b),cwd,v[2]))return -1;
    memset(&attrs,0,sizeof(attrs));attrs.flags=WOLFSSH_FILEATRB_PERM;attrs.per=0700;start=now();
    do {
        if(!strcmp(v[0],"mkdir")&&n==2)r=wolfSSH_SFTP_MKDIR(c->ssh,a,&attrs);
        else if(!strcmp(v[0],"rmdir")&&n==2)r=wolfSSH_SFTP_RMDIR(c->ssh,a);
        else if(!strcmp(v[0],"rm")&&n==2)r=wolfSSH_SFTP_Remove(c->ssh,a);
        else if(!strcmp(v[0],"rename")&&n==3)r=wolfSSH_SFTP_Rename(c->ssh,a,b);
        else if(!strcmp(v[0],"chmod")&&n==3){unsigned j;for(j=0;v[1][j];j++)if(v[1][j]<'0'||v[1][j]>'7')goto usage;if(j<3||j>4)goto usage;r=wolfSSH_SFTP_CHMOD(c->ssh,a,v[1]);}
        else goto usage;
        again=retry(c,r,start);
    }while(again>0);
    return again<0?-1:0;
usage:fputs("sftp: invalid command; type help\n",stderr);errno=EINVAL;return -1;
}
static int sftp_session(struct connection *c,const char *batch) {
    char cwd[SSH_PATH],line[2048],*v[5];WS_SFTPNAME *names;FILE *input=stdin;int n,result=0;
    names=remote_names(c,".",0);if(!names)return 1;
    n=copy_text(cwd,sizeof(cwd),names->fName);wolfSSH_SFTPNAME_list_free(names);if(n)return 1;
    if(batch){input=fopen(batch,"r");if(!input){perror("sftp: batch");return 1;}}
    while(!stopping) {
        if(!batch){fputs("sftp> ",stdout);fflush(stdout);}
        if(!fgets(line,sizeof(line),input)){if(ferror(input))result=1;break;}
        if(!strchr(line,'\n')&&!feof(input)){fputs("sftp: command too long\n",stderr);result=1;break;}
        n=words(line,v,5);if(n==1&&(!strcmp(v[0],"quit")||!strcmp(v[0],"bye")||!strcmp(v[0],"exit")))break;
        if(n<0||sftp_command(c,cwd,v,n)){if(n>=0)perror("sftp");result=1;if(batch)break;}
    }
    if(batch)fclose(input);return stopping?1:result;
}
static int scp_session(struct connection *c,char *source,char *destination,int recursive) {
    char endpoint[SSH_PATH],remote[SSH_PATH],local[SSH_PATH],*path;const char *name;int upload;
    WS_SFTP_FILEATRB attrs;struct stat st;
    /* A trailing source slash must not lose the directory's basename. */
    {char *colon=strchr(source,':'),*slash=strchr(source,'/');unsigned len=strlen(source),minimum=colon&&(!slash||colon<slash)?(unsigned)(colon-source)+2:1;
     while(len>minimum&&source[len-1]=='/')source[--len]=0;}
    upload=remote_name(source,endpoint,sizeof(endpoint),&path)==0;
    if(upload) {
        if(remote_name(destination,endpoint,sizeof(endpoint),&path)!=1)return 1;
        if(copy_text(remote,sizeof(remote),*path?path:"."))return 1;
        /* RealPath distinguishes directories without turning a missing new file
         * into an error on the normal upload path. */
        {unsigned len=strlen(remote);if(remote[len-1]=='/'||!strcmp(remote,".")){
            name=base_name(source);if(!name||path_join(local,sizeof(local),remote,name))return 1;copy_text(remote,sizeof(remote),local);
        }else {
            unsigned long start=now();int32_t r;int again;
            do{r=wolfSSH_SFTP_STAT(c->ssh,remote,&attrs);if(r<0&&!transient(r)&&!transient(wolfSSH_get_error(c->ssh)))break;again=retry(c,r,start);}while(again>0);
            if(r==0&&(attrs.flags&WOLFSSH_FILEATRB_PERM)&&(attrs.per&0170000)==0040000){name=base_name(source);if(!name||path_join(local,sizeof(local),remote,name))return 1;copy_text(remote,sizeof(remote),local);}
        }}
        if(recursive){if(remote_tree_path(c,remote)||copy_tree(c,remote,source,1,0)){perror("scp: recursive upload (tree may be partial)");return 1;}return 0;}
        return transfer(c,remote,source,1)?1:0;
    }
    if(remote_name(destination,endpoint,sizeof(endpoint),&path)!=0){fputs("scp: remote-to-remote copies are unsupported\n",stderr);return 1;}
    remote_name(source,endpoint,sizeof(endpoint),&path);if(copy_text(remote,sizeof(remote),*path?path:"."))return 1;
    if(stat(destination,&st)==0&&S_ISDIR(st.st_mode)) {
        name=base_name(remote);if(!name||path_join(local,sizeof(local),destination,name))return 1;
    }else if(copy_text(local,sizeof(local),destination))return 1;
    if(recursive){if(remote_tree_path(c,remote)||copy_tree(c,remote,local,0,0)){perror("scp: recursive download (tree may be partial)");return 1;}return 0;}
    return transfer(c,remote,local,0)?1:0;
}
