/* SPDX-License-Identifier: GPL-3.0-or-later */
struct settings {
    char alias[256],host[256],user[64],identity[SSH_PATH],known[SSH_PATH];
    unsigned port,timeout,strict,batch,tty,seen;
};
struct connection {
    struct settings config;
    struct ssh_key key;
    WOLFSSH_CTX *ctx;
    WOLFSSH *ssh;
    int fd,auth_used;
    unsigned long last_io;
    char pending[SSH_PUBLIC_LINE+384];
};
#define OPT_HOST 1
#define OPT_USER 2
#define OPT_PORT 4
#define OPT_IDENTITY 8
#define OPT_KNOWN 16
#define OPT_STRICT 32
#define OPT_BATCH 64
#define OPT_TIMEOUT 128
#define OPT_TTY 256
static int copy_text(char *to,unsigned cap,const char *from) {
    unsigned n=strlen(from);if(n>=cap){errno=ENAMETOOLONG;return -1;}memcpy(to,from,n+1);return 0;
}
static int glob(const char *p,const char *s) {
    while(*p) {
        if(*p=='*'){while(*p=='*')p++;if(!*p)return 1;do{if(glob(p,s))return 1;}while(*s++);return 0;}
        if(!*s||(*p!='?'&&tolower(*p)!=tolower(*s)))return 0;p++;s++;
    }
    return !*s;
}
static int option(struct settings *c,const char *key,const char *value) {
    unsigned bit=0;char *end;unsigned long n=0;char *dest=NULL;unsigned cap=0;
    if(!strcasecmp(key,"HostName")){bit=OPT_HOST;dest=c->host;cap=sizeof(c->host);}
    else if(!strcasecmp(key,"User")){bit=OPT_USER;dest=c->user;cap=sizeof(c->user);}
    else if(!strcasecmp(key,"IdentityFile")){bit=OPT_IDENTITY;dest=c->identity;cap=sizeof(c->identity);}
    else if(!strcasecmp(key,"UserKnownHostsFile")){bit=OPT_KNOWN;dest=c->known;cap=sizeof(c->known);}
    else if(!strcasecmp(key,"Port"))bit=OPT_PORT;
    else if(!strcasecmp(key,"ConnectTimeout"))bit=OPT_TIMEOUT;
    else if(!strcasecmp(key,"StrictHostKeyChecking"))bit=OPT_STRICT;
    else if(!strcasecmp(key,"BatchMode"))bit=OPT_BATCH;
    else if(!strcasecmp(key,"RequestTTY"))bit=OPT_TTY;
    else if(!strcasecmp(key,"IdentitiesOnly")){if(!strcasecmp(value,"yes"))return 0;goto bad;}
    else goto bad;
    if(c->seen&bit)return 0;
    if(dest) {
        if(value[0]=='~'&&value[1]=='/') {
            const char *home=getenv("HOME");int r;if(!home||home[0]!='/')home="/root";
            r=snprintf(dest,cap,"%s/%s",home,value+2);if(r<0||(unsigned)r>=cap)goto bad;
        } else if(copy_text(dest,cap,value))return -1;
    } else if(bit==OPT_PORT||bit==OPT_TIMEOUT) {
        n=strtoul(value,&end,10);if(!*value||*end||!n||n>(bit==OPT_PORT?65535UL:600UL))goto bad;
        if(bit==OPT_PORT)c->port=n;else c->timeout=n;
    } else if(bit==OPT_STRICT) {
        if(!strcasecmp(value,"yes"))c->strict=1;else if(!strcasecmp(value,"ask"))c->strict=0;
        else if(!strcasecmp(value,"accept-new"))c->strict=2;else goto bad;
    } else if(bit==OPT_BATCH) {
        if(!strcasecmp(value,"yes"))c->batch=1;else if(!strcasecmp(value,"no"))c->batch=0;else goto bad;
    } else {
        if(!strcasecmp(value,"yes")||!strcasecmp(value,"force"))c->tty=2;
        else if(!strcasecmp(value,"no"))c->tty=1;else if(!strcasecmp(value,"auto"))c->tty=0;else goto bad;
    }
    c->seen|=bit;return 0;
bad:fprintf(stderr,"ssh: unsupported or invalid option: %s %s\n",key,value);errno=EINVAL;return -1;
}
static int command_option(struct settings *c,char *text) {
    char buf[SSH_PATH],*value;unsigned old=c->seen;
    if(copy_text(buf,sizeof(buf),text))return -1;
    value=strchr(buf,'=');if(!value){value=buf;while(*value&&*value!=' '&&*value!='\t')value++;}
    if(!*value)return -1;*value++=0;while(*value==' '||*value=='\t')value++;
    /* CLI options override config; the last CLI occurrence wins. */
    c->seen=0;if(option(c,buf,value)){c->seen=old;return -1;}c->seen|=old;return 0;
}
/* Bounded shell-style quoting for config and sftp commands, without expansion. */
static int words(char *line,char **argv,unsigned cap) {
    char *p=line,*out=line;unsigned n=0;char quote;
    while(*p) {
        while(*p==' '||*p=='\t'||*p=='\r'||*p=='\n')p++;
        if(!*p||*p=='#')break;if(n==cap)return -1;
        argv[n++]=out;quote=0;
        while(*p) {
            char ch=*p++;
            if(!quote&&(ch==' '||ch=='\t'||ch=='\r'||ch=='\n'))break;
            if(ch=='\\'&&quote!='\''){if(!*p)return -1;*out++=*p++;}
            else if(ch=='\''||ch=='"'){if(quote==ch)quote=0;else if(!quote)quote=ch;else *out++=ch;}
            else *out++=ch;
        }
        if(quote)return -1;*out++=0;
    }
    return n;
}
static int load_config(struct settings *c,const char *path,int required) {
    byte *data;unsigned size,line=0;char *p,*next,*argv[10];int n,active=1;unsigned i;
    data=ssh_read_file(path,&size,0);if(!data)return !required&&errno==ENOENT?0:-1;
    p=(char *)data;
    while(*p) {
        line++;next=strchr(p,'\n');if(next)*next++=0;
        /* Permit OpenSSH's optional '=' separator. */
        {char *eq=p;while(*eq==' '||*eq=='\t')eq++;while(*eq&&*eq!='='&&*eq!=' '&&*eq!='\t')eq++;while(*eq==' '||*eq=='\t')eq++;if(*eq=='=')*eq=' ';}
        n=words(p,argv,10);if(n<0)goto bad;
        if(n&&!strcasecmp(argv[0],"Host")) {
            int matched=0,denied=0;
            for(i=1;i<(unsigned)n;i++)if(argv[i][0]=='!'){if(glob(argv[i]+1,c->alias))denied=1;}
            else if(glob(argv[i],c->alias))matched=1;
            active=matched&&!denied;
        } else if(n&&active) {if(n!=2||option(c,argv[0],argv[1]))goto bad;}
        if(!next)break;p=next;
    }
    free(data);return 0;
bad:fprintf(stderr,"ssh: invalid configuration at %s:%u\n",path,line);free(data);errno=EINVAL;return -1;
}
static int hashed_host(const char *entry,const char *host) {
    char copy[256],*split;byte salt[64],wanted[20],actual[20];word32 ns=sizeof(salt),nw=sizeof(wanted);Hmac h;int r=-1;
    if(strncmp(entry,"|1|",3)||copy_text(copy,sizeof(copy),entry+3))return 0;
    split=strchr(copy,'|');if(!split)return 0;*split++=0;
    if(Base64_Decode((byte *)copy,strlen(copy),salt,&ns)||Base64_Decode((byte *)split,strlen(split),wanted,&nw)||nw!=20)return 0;
    if(wc_HmacInit(&h,NULL,INVALID_DEVID))return 0;
    if(!wc_HmacSetKey(&h,WC_SHA,salt,ns)&&!wc_HmacUpdate(&h,(byte *)host,strlen(host))&&!wc_HmacFinal(&h,actual))r=memcmp(wanted,actual,20);
    wc_HmacFree(&h);return r==0;
}
static int hosts_match(char *entry,const char *host) {
    int found=0;char *p=entry,*next;
    if(*p=='|')return hashed_host(p,host);
    while(p) {next=strchr(p,',');if(next)*next++=0;if(*p=='!'){if(glob(p+1,host))return 0;}else if(glob(p,host))found=1;p=next;}
    return found;
}
static int host_key(const byte *blob,word32 length,void *context) {
    struct connection *c=context;byte *data=NULL,decoded[SSH_BLOB_BYTES];unsigned size=0,type,i;word32 n;
    char host[320],algorithm[64],finger[80],line[SSH_PUBLIC_LINE],*p,*next,*argv[5];int found=0,changed=0,revoked=0,count;
    if(length<4||length>SSH_BLOB_BYTES||(type=ssh_be32(blob))>=sizeof(algorithm)||type>length-4)return -1;
    memcpy(algorithm,blob+4,type);algorithm[type]=0;
    if(c->config.port==22)copy_text(host,sizeof(host),c->config.host);else snprintf(host,sizeof(host),"[%s]:%u",c->config.host,c->config.port);
    data=ssh_read_file(c->config.known,&size,0);if(!data&&errno!=ENOENT){perror("ssh: known_hosts");return -1;}
    p=(char *)data;
    while(p&&*p) {
        next=strchr(p,'\n');if(next)*next++=0;
        /* The remainder is an arbitrary comment, not additional fields. */
        {char *end=p;unsigned fields=0,need;while(*end==' '||*end=='\t')end++;need=*end=='@'?4:3;
         while(*end&&fields<need){while(*end&&*end!=' '&&*end!='\t')end++;fields++;if(fields==need){*end=0;break;}while(*end==' '||*end=='\t')end++;}}
        count=words(p,argv,5);
        if(count>=3) {
            int mark=argv[0][0]=='@',offset=mark?1:0;
            if(count>=offset+3&&hosts_match(argv[offset],host)&&!strcmp(argv[offset+1],algorithm)) {
                n=sizeof(decoded);
                if(Base64_Decode((byte *)argv[offset+2],strlen(argv[offset+2]),decoded,&n)){free(data);return -1;}
                if(n==length&&!memcmp(decoded,blob,n)){if(mark&&!strcmp(argv[0],"@revoked"))revoked=1;else if(!mark)found=1;}
                else if(!mark)changed=1;
            }
        } else if(count<0){free(data);return -1;}
        p=next;
    }
    free(data);
    if(revoked){fputs("ssh: server host key is revoked\n",stderr);return -1;}
    if(found)return 0;
    if(ssh_fingerprint(blob,length,finger,sizeof(finger)))return -1;
    if(changed){fprintf(stderr,"ssh: HOST KEY CHANGED for %s\n%s\n",host,finger);return -1;}
    if(c->config.strict==1||(c->config.strict==0&&c->config.batch)){fprintf(stderr,"ssh: unknown host %s (%s)\n",host,finger);return -1;}
    if(c->config.strict==0) {
        char answer[16];fprintf(stderr,"Host %s\n%s %s\n",host,algorithm,finger);
        /* Trust confirmation is visible; never consume remote stdin. */
        int fd=open("/dev/tty",O_RDWR),at=0,over=0;char ch;
        const char *prompt="Trust this host key? Type yes: ";
        if(fd<0)return -1;
        if(write(fd,prompt,strlen(prompt))<0){close(fd);return -1;}
        while(read(fd,&ch,1)==1&&ch!='\n'&&ch!='\r'){if(at<15)answer[at++]=ch;else over=1;}
        close(fd);answer[at]=0;if(over||strcmp(answer,"yes"))return -1;
    }
    i=ssh_public_line(blob,length,"",line,sizeof(line));if((int)i<0)return -1;
    if(snprintf(c->pending,sizeof(c->pending),"%s %s",host,line)>=sizeof(c->pending))return -1;
    return 0;
}
static int save_host(struct connection *c) {
    byte *old,*data;unsigned n=0,add=strlen(c->pending),at;int exists,r;
    if(!add)return 0;
    old=ssh_read_file(c->config.known,&n,0);exists=old!=NULL;if(!old&&errno!=ENOENT)return -1;
    if(n+add+1>16384){free(old);errno=EFBIG;return -1;}
    data=malloc(n+add+2);if(!data){free(old);return -1;}if(n)memcpy(data,old,n);at=n;
    if(at&&data[at-1]!='\n')data[at++]='\n';memcpy(data+at,c->pending,add);at+=add;
    r=ssh_write_file(c->config.known,data,at,0600,exists);free(data);free(old);if(!r)c->pending[0]=0;return r;
}
