/* SPDX-License-Identifier: GPL-3.0-or-later */
/* Guest-owned SFTP filesystem. Reuse exactly the client's trust/key policy. */
#define main ssh_command_main
#include "client.c"
#undef main
#include "remotefs.h"
#include <sys/wait.h>
extern long call(unsigned,long,long,long,long);
#include "mount-wire.inc"
#define NODE_LIMIT 512
struct remote_node {
    char path[SSH_PATH];
    unsigned long info[8],sample;
    byte handle[256];
    unsigned handle_size,writeable,held,dirty,cached,append,unlinked;
};
struct filesystem {
    struct wire wire;
    struct remote_node nodes[NODE_LIMIT];
    unsigned listing_node,listing_size;
    unsigned char listing[NODE_LIMIT*32];
    unsigned long listing_sample;
};
static int path_under(const char *path,const char *parent) {
    unsigned n=strlen(parent);return !strncmp(path,parent,n)&&(!path[n]||path[n]=='/');
}
static int node_number(struct filesystem *f,const char *path,const unsigned long *info) {
    unsigned i,free=0;unsigned long oldest=now();
    for(i=1;i<NODE_LIMIT;i++) {
        if(!f->nodes[i].unlinked&&!strcmp(f->nodes[i].path,path))break;
        if(i>1&&!f->nodes[i].held&&!f->nodes[i].cached&&!f->nodes[i].handle_size &&
           (!free||!f->nodes[i].path[0]||f->nodes[i].sample<oldest)) {
            free=i;oldest=f->nodes[i].sample;
        }
    }
    if(i==NODE_LIMIT) {
        if(!free){errno=ENFILE;return -1;}i=free;
        memset(&f->nodes[i],0,sizeof(f->nodes[i]));
        if(copy_text(f->nodes[i].path,SSH_PATH,path))return -1;
    }
    if(info){memcpy(f->nodes[i].info,info,8*sizeof(unsigned long));f->nodes[i].sample=now();}
    return i;
}
static int stat_node(struct filesystem *f,unsigned n,int force) {
    struct remote_node *p=&f->nodes[n];
    if(!force&&p->sample&&now()-p->sample<2000UL)return 0;
    if(p->handle_size) {
        begin(&f->wire,WOLFSSH_FTP_FSTAT);putstr(&f->wire,p->handle,p->handle_size);
        if(exchange(&f->wire,WOLFSSH_FTP_ATTRS)||attrs(&f->wire,p->info))return -1;
    } else if(wire_stat(&f->wire,p->path,p->info))return -1;p->sample=now();return 0;
}
static int close_node(struct filesystem *,unsigned);
static int open_node(struct filesystem *f,unsigned n,int access,int append) {
    struct remote_node *p=&f->nodes[n];struct wire *w=&f->wire;
    if((p->info[0]&0170000)==0040000)return 0;
    if((p->info[0]&0170000)!=0100000){errno=EOPNOTSUPP;return -1;}
    if(p->handle_size&&(p->writeable&(unsigned)access)==(unsigned)access&&(!(access&2)||p->append==(unsigned)append))return 0;
    /* Separate read-only and write-only permissions must remain usable. */
    if(p->unlinked){errno=EOPNOTSUPP;return -1;}
    if(p->handle_size){access|=p->writeable;if(close_node(f,n))return -1;}
    begin(w,WOLFSSH_FTP_OPEN);putstr(w,p->path,strlen(p->path));
    put32(w,((access&2)?WOLFSSH_FXF_WRITE:0)|((access&1)?WOLFSSH_FXF_READ:0)|(append?WOLFSSH_FXF_APPEND:0));put32(w,0);
    if(wire_handle(w,p->handle,&p->handle_size))return -1;p->writeable=access;p->append=append;return 0;
}
static int close_node(struct filesystem *f,unsigned n) {
    struct remote_node *p=&f->nodes[n];int r=0,saved;
    if(p->handle_size) {
        if(p->dirty&&wire_fsync(&f->wire,p->handle,p->handle_size))r=-1;
        saved=errno;
        if(wire_close(&f->wire,p->handle,p->handle_size))r=-1;
        else if(r)errno=saved;
        p->handle_size=p->writeable=p->dirty=0;
    }
    return r;
}
static int name_path(struct filesystem *f,unsigned parent,const char *name,char *out) {
    unsigned i;
    if(!parent||parent>=NODE_LIMIT||!f->nodes[parent].path[0]){errno=ENOENT;return -1;}
    if(!*name||strlen(name)>30||strchr(name,'/')){errno=EINVAL;return -1;}
    for(i=0;name[i];i++)if((unsigned)name[i]>255){errno=EINVAL;return -1;}
    if(!strcmp(name,"."))return copy_text(out,SSH_PATH,f->nodes[parent].path);
    if(!strcmp(name,"..")) {
        if(copy_text(out,SSH_PATH,f->nodes[parent].path))return -1;
        if(parent!=1){char *p=strrchr(out,'/');if(p&&p!=out)*p=0;else out[1]=0;}
        return 0;
    }
    return path_join(out,SSH_PATH,f->nodes[parent].path,name);
}
static void invalidate(struct filesystem *f) {
    unsigned i;f->listing_node=0;
    for(i=1;i<NODE_LIMIT;i++){f->nodes[i].sample=0;f->nodes[i].cached=0;}
}
/* One complete cached directory snapshot, including READDIR attributes. The
 * following lookups/stat calls reuse these attributes, avoiding N round trips. */
static int directory(struct filesystem *f,unsigned node,int force) {
    struct wire *w=&f->wire;struct remote_node *p=&f->nodes[node];
    byte handle[256];unsigned length=0,used=0,i,j;int result=-1,saved;
    unsigned long count,n,ignored,info[8];byte *name;char component[31],path[SSH_PATH];
    if(!force&&f->listing_node==node)return 0;
    f->listing_node=0;for(i=1;i<NODE_LIMIT;i++)f->nodes[i].cached=0;begin(w,WOLFSSH_FTP_OPENDIR);putstr(w,p->path,strlen(p->path));
    if(wire_handle(w,handle,&length))return -1;
    for(;;) {
        begin(w,WOLFSSH_FTP_READDIR);putstr(w,handle,length);
        if(exchange(w,WOLFSSH_FTP_NAME)){if(errno==ENXIO){result=0;break;}goto done;}
        count=get32(w);if(w->bad||count>NODE_LIMIT){errno=EOVERFLOW;goto done;}
        if(!count){errno=EIO;goto done;}
        for(i=0;i<(unsigned)count;i++) {
            name=getstr(w,&n);if(w->bad)goto malformed;
            if(!n||n>30){errno=ENAMETOOLONG;goto done;}
            for(j=0;j<(unsigned)n;j++)if(!name[j]||name[j]=='/'||name[j]>255)goto malformed;
            memcpy(component,name,n);component[n]=0;
            getstr(w,&ignored);if(attrs(w,info))goto done;
            if(used>=NODE_LIMIT){errno=EOVERFLOW;goto done;}
            if(name_path(f,node,component,path))goto done;
            j=!strcmp(component,".")?node:!strcmp(component,"..")&&node==1?1:node_number(f,path,info);
            if((int)j<0)goto done;f->nodes[j].cached=1;
            memset(f->listing+used*32,0,32);f->listing[used*32]=j;
            memcpy(f->listing+used*32+1,component,n);used++;
        }
    }
    goto done;
malformed:w->dead=1;errno=EIO;
done:
    saved=errno;if(wire_close(w,handle,length))return -1;errno=saved;
    if(!result){f->listing_node=node;f->listing_size=used*32;f->listing_sample=now();}
    return result;
}
static long service(struct filesystem *f,struct rfs_message *q) {
    struct wire *w=&f->wire;unsigned n=q->arg[0],op=q->operation,count;
    struct remote_node *p;char a[SSH_PATH],b[SSH_PATH];unsigned long info[8],length,start;
    byte handle[256],*bytes;unsigned hlen=0,i;int number,r;
    if(op==13)n=1;
    if(n<1||n>=NODE_LIMIT||!f->nodes[n].path[0]){errno=ENOENT;return -1;}p=&f->nodes[n];
    if(op==1) {
        if(name_path(f,n,q->name,a))return -1;
        number=node_number(f,a,NULL);if(number<0)return -1;
        if(stat_node(f,number,0))return -1;
        if((f->nodes[number].info[0]&0170000)!=0100000 &&
           (f->nodes[number].info[0]&0170000)!=0040000){errno=EOPNOTSUPP;return -1;}
        f->nodes[number].held=1;return number;
    }
    if(op==2) {if(stat_node(f,n,0))return -1;memcpy(q->arg,p->info,sizeof(p->info));return 0;}
    if(op==3&&(p->info[0]&0170000)==0040000) {
        if(directory(f,n,q->arg[1]==0))return -1;
        start=q->arg[1];if(start>=f->listing_size)return 0;
        count=f->listing_size-start;if(count>q->arg[3])count=q->arg[3];
        memcpy(q->data,f->listing+start,count);return count;
    }
    if(op==3||op==4) {
        if(q->arg[3]>RFS_BYTES||q->arg[1]>2147483647UL){errno=EFBIG;return -1;}
        start=q->arg[1];count=q->arg[3];
        if(op==4&&q->arg[4]){if(stat_node(f,n,1))return -1;start=p->info[2];}
        if(start>2147483647UL-count){errno=EFBIG;return -1;}
        if(open_node(f,n,op==4?2:1,op==4&&q->arg[4]))return -1;
        begin(w,op==3?WOLFSSH_FTP_READ:WOLFSSH_FTP_WRITE);putstr(w,p->handle,p->handle_size);
        put32(w,0);put32(w,start);
        if(op==3) {
            put32(w,count);if(exchange(w,WOLFSSH_FTP_DATA))return errno==ENXIO?0:-1;
            bytes=getstr(w,&length);if(w->bad||length>count){w->dead=1;errno=EIO;return -1;}
            memcpy(q->data,bytes,length);return length;
        }
        for(i=0;i<count;i++)if(q->data[i]>255){errno=EINVAL;return -1;}
        putstr(w,q->data,count);if(exchange(w,WOLFSSH_FTP_STATUS))return -1;
        p->dirty=1;p->sample=0;q->arg[5]=start+count;
        if(q->arg[4]){if(stat_node(f,n,1))return -1;q->arg[5]=p->info[2];}
        if(q->arg[6]){if(wire_fsync(w,p->handle,p->handle_size))return -1;p->dirty=0;}
        return count;
    }
    if(op==5) {
        if(name_path(f,n,q->name,a))return -1;
        begin(w,q->arg[3]?WOLFSSH_FTP_MKDIR:WOLFSSH_FTP_OPEN);putstr(w,a,strlen(a));
        if(!q->arg[3])put32(w,WOLFSSH_FXF_WRITE|WOLFSSH_FXF_CREAT|WOLFSSH_FXF_EXCL);
        put32(w,4);put32(w,q->arg[2]&0777);
        if(q->arg[3])r=exchange(w,WOLFSSH_FTP_STATUS);
        else {r=wire_handle(w,handle,&hlen);if(!r)r=wire_close(w,handle,hlen);}
        if(r)return -1;invalidate(f);
        if(wire_stat(w,a,info))return -1;number=node_number(f,a,info);
        if(number>0)f->nodes[number].held=1;return number;
    }
    if(op==6) {
        if(name_path(f,n,q->name,a))return -1;
        begin(w,q->arg[2]?WOLFSSH_FTP_RMDIR:WOLFSSH_FTP_REMOVE);putstr(w,a,strlen(a));
        r=exchange(w,WOLFSSH_FTP_STATUS);
        if(!r){for(i=1;i<NODE_LIMIT;i++)if(!strcmp(f->nodes[i].path,a))f->nodes[i].unlinked=1;invalidate(f);}return r;
    }
    if(op==8) {
        if(name_path(f,n,q->name,a)||name_path(f,q->arg[2],q->name2,b))return -1;
        for(i=1;i<NODE_LIMIT;i++)if(!f->nodes[i].unlinked&&path_under(f->nodes[i].path,a)) {
            if(strlen(b)+strlen(f->nodes[i].path)-strlen(a)>=SSH_PATH){errno=ENAMETOOLONG;return -1;}
        }
        /* Never emulate overwrite with remove+rename: failed saves must leave
         * the original intact. Unsupported servers fail this operation. */
        begin(w,WOLFSSH_FTP_EXTENDED);putstr(w,"posix-rename@openssh.com",24);
        putstr(w,a,strlen(a));putstr(w,b,strlen(b));
        if(exchange(w,WOLFSSH_FTP_STATUS))return -1;
        if(strcmp(a,b))for(i=1;i<NODE_LIMIT;i++)if(!strcmp(f->nodes[i].path,b))f->nodes[i].unlinked=1;
        for(i=1;i<NODE_LIMIT;i++)if(!f->nodes[i].unlinked&&path_under(f->nodes[i].path,a)) {
            char updated[SSH_PATH];
            r=snprintf(updated,sizeof(updated),"%s%s",b,f->nodes[i].path+strlen(a));
            if(r<0||(unsigned)r>=sizeof(updated)){w->dead=1;errno=ENAMETOOLONG;return -1;}
            strcpy(f->nodes[i].path,updated);
        }
        invalidate(f);return 0;
    }
    if(op==9||op==10||op==11) {
        if(op==9&&q->arg[1]>2147483647UL){errno=EFBIG;return -1;}
        begin(w,p->handle_size?WOLFSSH_FTP_FSETSTAT:WOLFSSH_FTP_SETSTAT);
        if(p->handle_size)putstr(w,p->handle,p->handle_size);else putstr(w,p->path,strlen(p->path));
        put32(w,op==9?1:op==10?4:8);
        if(op==9)put32(w,0);put32(w,q->arg[1]);if(op==11)put32(w,q->arg[2]);
        if(exchange(w,WOLFSSH_FTP_STATUS))return -1;invalidate(f);return 0;
    }
    if(op==12){r=close_node(f,n);p->held=0;return r;}
    if(op==13) {
        for(i=1;i<NODE_LIMIT;i++)if(f->nodes[i].dirty) {
            if(wire_fsync(w,f->nodes[i].handle,f->nodes[i].handle_size))return -1;f->nodes[i].dirty=0;
        }
        return 0;
    }
    if(op==14)return open_node(f,n,q->arg[1]==O_RDWR?3:q->arg[1]==O_WRONLY?2:1,q->arg[2]);
    if(op==15) {
        if(directory(f,n,1))return -1;
        for(i=0;i<f->listing_size/32;i++) {
            char name[31];memcpy(name,f->listing+i*32+1,30);name[30]=0;
            if(strcmp(name,".")&&strcmp(name,".."))return 0;
        }
        return 1;
    }
    errno=EOPNOTSUPP;return -1;
}
static int setup_connection(struct connection *c,char *endpoint,const char *configuration) {
    char config[SSH_PATH],dir[SSH_PATH],*user;struct stat st;unsigned i;
    c->fd=-1;c->config.port=22;c->config.timeout=30;
    user=strchr(endpoint,'@');if(user){*user++=0;if(option(&c->config,"User",endpoint))return -1;memmove(endpoint,user,strlen(user)+1);}
    if(!*endpoint||strchr(endpoint,':')||strchr(endpoint,'/')||copy_text(c->config.alias,sizeof(c->config.alias),endpoint)){errno=EINVAL;return -1;}
    if(ssh_directory(dir,sizeof(dir))||ssh_path(config,sizeof(config),"config"))return -1;
    if(load_config(&c->config,configuration?configuration:config,configuration!=NULL))return -1;
    if(!c->config.host[0])strcpy(c->config.host,endpoint);
    if(!c->config.user[0]){user=getenv("USER");if(copy_text(c->config.user,sizeof(c->config.user),user?user:"root"))return -1;}
    if(!c->config.identity[0]) {
        const char *names[]={"id_ecdsa","id_ed25519","id_rsa"};
        for(i=0;i<3;i++){if(ssh_path(c->config.identity,sizeof(c->config.identity),names[i]))return -1;if(!lstat(c->config.identity,&st))break;if(errno!=ENOENT)return -1;}
    }
    if(!c->config.known[0]&&ssh_path(c->config.known,sizeof(c->config.known),"known_hosts"))return -1;
    if(wolfSSH_Init()!=WS_SUCCESS){errno=EIO;return -1;}ripple_crypto_init();
    if(ssh_key_load(&c->key,c->config.identity,c->config.batch?"":NULL))return -1;
    return connect_server(c,1,0,NULL);
}
int main(int argc,char **argv) {
    struct filesystem *f=NULL;struct connection *c=NULL;struct rfs_message *q=NULL;
    char endpoint[SSH_PATH],root[SSH_PATH];char *path,*config=NULL;int mount=-1,result=1,at=1,i,background=0,parent_done=0,ready[2]={-1,-1};
    if(argc>=2&&!strcmp(argv[1],"--status")) {
        for(i=0;i<RFS_MOUNTS;i++){struct rfs_status st;long r=call(RFS_SYSCALL,RFS_STATUS,i,(long)&st,0);if(r<0){perror("sftp-mount");return 1;}if(r)printf("%d: %s %s (PID %u)\n",i,r==1?"connected":"disconnected",st.path,st.pid);}return 0;
    }
    if(argc==3&&!strcmp(argv[1],"--unmount")) {
        char *end;long n=strtol(argv[2],&end,10);if(*end||n<0||n>=RFS_MOUNTS)goto usage;
        if(call(RFS_SYSCALL,RFS_DETACH,n,0,0)<0){perror("sftp-mount: unmount");return 1;}return 0;
    }
    if(argc>2&&!strcmp(argv[1],"--background")){background=1;at++;}
    if(argc-at>2&&!strcmp(argv[at],"-F")){config=argv[at+1];at+=2;}
    if(argc-at!=2)goto usage;
    f=calloc(1,sizeof(*f));c=calloc(1,sizeof(*c));q=calloc(1,sizeof(*q));
    if(!f||!c||!q)goto done;c->fd=-1;
    f->wire.connection=c;f->wire.receive=malloc(65535UL);if(!f->wire.receive)goto done;
    if(remote_name(argv[at],endpoint,sizeof(endpoint),&path)!=1||!*path)goto usage;
    if(path[0]!='/'||copy_text(root,sizeof(root),path)||local_path(argv[at+1]))goto failed;
    signal(SIGPIPE,SIG_IGN);signal(SIGINT,caught);signal(SIGTERM,caught);signal(SIGHUP,caught);
    if(setup_connection(c,endpoint,config))goto failed;
    /* Resolve to a canonical directory before publishing a mount. */
    {WS_SFTPNAME *names=remote_names(c,root,0);if(!names)goto failed;
        i=copy_text(f->nodes[1].path,SSH_PATH,names->fName);wolfSSH_SFTPNAME_list_free(names);if(i)goto failed;}
    if(stat_node(f,1,1)||(f->nodes[1].info[0]&0170000)!=0040000){errno=ENOTDIR;goto failed;}
    f->nodes[1].held=1;
    if(background) {
        int pid,status;unsigned nread=0;char *bytes=(char *)&mount;
        if(pipe(ready))goto failed;pid=fork();if(pid<0)goto failed;
        if(pid) {
            close(ready[1]);ready[1]=-1;
            while(nread<sizeof(mount)) {
                int got=read(ready[0],bytes+nread,sizeof(mount)-nread);
                if(got<0&&errno==EINTR)continue;if(got<=0)break;nread+=got;
            }
            close(ready[0]);ready[0]=-1;parent_done=1;
            if(nread!=sizeof(mount)||mount<0){while(waitpid(pid,&status,0)<0&&errno==EINTR);errno=EIO;goto failed;}
            result=0;goto done;
        }
        close(ready[0]);ready[0]=-1;
        if(setsid()<0)goto failed;
        signal(SIGINT,SIG_IGN);signal(SIGQUIT,SIG_IGN);signal(SIGHUP,SIG_IGN);
    }
    mount=call(RFS_SYSCALL,RFS_ATTACH,(long)argv[at+1],0,0);if(mount<0)goto failed;
    printf("SFTP mounted at %s (attachment %d)\n",argv[at+1],mount);fflush(stdout);
    if(ready[1]>=0){
        int nullfd;
        if(write(ready[1],&mount,sizeof(mount))!=sizeof(mount))goto failed;
        close(ready[1]);ready[1]=-1;
        nullfd=open("/dev/null",O_RDWR);
        if(nullfd<0)goto failed;
        if(dup2(nullfd,0)<0||dup2(nullfd,1)<0||dup2(nullfd,2)<0){close(nullfd);goto failed;}
        if(nullfd>2)close(nullfd);
    }
    while(!stopping&&!f->wire.dead) {
        if(call(RFS_SYSCALL,RFS_NEXT,mount,(long)q,0)<0){if(errno==EINTR&&!stopping)continue;if(errno==ENOTCONN){result=0;break;}goto failed;}
        if(q->version!=RFS_VERSION||q->mount!=(unsigned)mount||q->arg[3]>0x7fffffffUL){errno=EIO;goto failed;}
        errno=0;q->result=service(f,q);if(q->result<0)q->result=-(errno?errno:EIO);
        if(call(RFS_SYSCALL,RFS_REPLY,mount,(long)q,0)<0)goto failed;
    }
    if(stopping)result=128+stopping;
    goto done;
failed:perror("sftp-mount");goto done;
usage:fputs("usage: sftp-mount [--background] [-F config] [user@]host:/directory /mnt/name\n       sftp-mount --status | --unmount NUMBER\nKeep running (or use &); exit the mount directory before unmounting.\n",stderr);
    result=2;
done:
    if(ready[0]>=0)close(ready[0]);if(ready[1]>=0)close(ready[1]);
    if(f&&c&&c->ssh&&!f->wire.dead&&!parent_done){for(i=1;i<NODE_LIMIT;i++)if(close_node(f,i))result=1;}
    if(c){if(parent_done){if(c->ssh)wolfSSH_free(c->ssh);if(c->ctx)wolfSSH_CTX_free(c->ctx);if(c->fd>=0)close(c->fd);}else disconnect(c);ssh_wipe(c,sizeof(*c));free(c);}if(f){free(f->wire.receive);free(f);}free(q);wolfSSH_Cleanup();return result;
}
