/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SFTPv3 requests for the filesystem service. Exactly one request is in flight;
 * every response is bounded and matched to its request before parsing. */
struct wire {
    struct connection *connection;
    byte send[8192];
    byte *receive;
    unsigned long length,at,size,id;
    int bad,dead;
};
static void put32(struct wire *w,unsigned long value) {
    unsigned i;if(w->length+4>sizeof(w->send)){w->bad=1;return;}
    for(i=0;i<4;i++)w->send[w->length++]=(value>>(24-8*i))&255;
}
static void putstr(struct wire *w,const void *data,unsigned length) {
    put32(w,length);if(w->length+length>sizeof(w->send)){w->bad=1;return;}
    memcpy(w->send+w->length,data,length);w->length+=length;
}
static unsigned long get32(struct wire *w) {
    unsigned i;unsigned long value=0;
    if(w->at+4>w->size){w->bad=1;return 0;}
    for(i=0;i<4;i++)value=(value<<8)|w->receive[w->at++];return value;
}
static byte *getstr(struct wire *w,unsigned long *length) {
    byte *p;*length=get32(w);
    if(w->bad||*length>w->size-w->at){w->bad=1;return NULL;}
    p=w->receive+w->at;w->at+=*length;return p;
}
static void begin(struct wire *w,unsigned type) {
    w->length=0;w->bad=0;put32(w,0);w->send[w->length++]=type;put32(w,++w->id);
}
static int transport(struct wire *w,byte *buffer,unsigned long size,int writing) {
    unsigned long at=0,start=now();int n,e;
    while(at<size&&!stopping) {
        unsigned nbytes=size-at>4096UL?4096:(unsigned)(size-at);
        n=writing?wolfSSH_stream_send(w->connection->ssh,buffer+at,nbytes):wolfSSH_stream_read(w->connection->ssh,buffer+at,nbytes);
        if(n>0){if(n>nbytes)break;at+=n;start=now();continue;}
        e=wolfSSH_get_error(w->connection->ssh);
        if(!transient(n)&&!(n==WS_FATAL_ERROR&&transient(e)))break;
        if(wait_socket(w->connection,start,30000UL))break;
    }
    if(at!=size){w->dead=1;errno=stopping?EINTR:ENOTCONN;return -1;}return 0;
}
static int status_error(unsigned long status) {
    if(status==0)return 0;
    errno=status==1?ENXIO:status==2?ENOENT:status==3?EACCES:
        status==6||status==7?ENOTCONN:status==8?EOPNOTSUPP:EIO;
    return -1;
}
static int exchange(struct wire *w,unsigned expected) {
    unsigned long size;unsigned i;
    if(w->bad){errno=EOVERFLOW;return -1;}
    size=w->length-4;for(i=0;i<4;i++)w->send[i]=(size>>(24-8*i))&255;
    if(transport(w,w->send,w->length,1)||transport(w,w->receive,4,0))return -1;
    w->at=0;w->size=4;size=get32(w);
    if(size<5||size>65535UL){w->dead=1;errno=EIO;return -1;}
    if(transport(w,w->receive,size,0))return -1;
    w->size=size;w->at=1;
    if(get32(w)!=w->id){w->dead=1;errno=EIO;return -1;}
    if(w->receive[0]==WOLFSSH_FTP_STATUS) {
        unsigned long status=get32(w);
        if(w->bad){w->dead=1;errno=EIO;return -1;}
        if(!status && expected!=WOLFSSH_FTP_STATUS){w->dead=1;errno=EIO;return -1;}
        return status_error(status);
    }
    if(w->receive[0]!=expected){w->dead=1;errno=EIO;return -1;}
    return 0;
}
static int attrs(struct wire *w,unsigned long *info) {
    unsigned long flags=get32(w),high,low,n,length;
    memset(info,0,8*sizeof(unsigned long));info[1]=1;
    if(flags&~0x8000000fUL){w->bad=1;return -1;}
    if(flags&1){high=get32(w);low=get32(w);if(high||low>2147483647UL){errno=EFBIG;return -1;}info[2]=low;}
    if(flags&2){get32(w);get32(w);}
    if(flags&4)info[0]=get32(w)&0177777UL;
    if(flags&8){info[3]=get32(w);info[4]=get32(w);info[5]=info[4];}
    if(flags&0x80000000UL){n=get32(w);if(n>1024){w->bad=1;return -1;}while(n--){getstr(w,&length);getstr(w,&length);}}
    if(w->bad){w->dead=1;errno=EIO;return -1;}
    if(!(flags&4)){errno=EOPNOTSUPP;return -1;}
    if((info[0]&0170000)==0040000)info[1]=2;
    return 0;
}
static int wire_stat(struct wire *w,const char *path,unsigned long *info) {
    begin(w,WOLFSSH_FTP_LSTAT);putstr(w,path,strlen(path));
    if(exchange(w,WOLFSSH_FTP_ATTRS))return -1;
    return attrs(w,info);
}
static int wire_handle(struct wire *w,byte *handle,unsigned *length) {
    unsigned long n;byte *p;
    if(exchange(w,WOLFSSH_FTP_HANDLE))return -1;
    p=getstr(w,&n);if(w->bad||!n||n>256){w->dead=1;errno=EIO;return -1;}
    memcpy(handle,p,n);*length=n;return 0;
}
static int wire_close(struct wire *w,byte *handle,unsigned length) {
    begin(w,WOLFSSH_FTP_CLOSE);putstr(w,handle,length);return exchange(w,WOLFSSH_FTP_STATUS);
}
static int wire_fsync(struct wire *w,byte *handle,unsigned length) {
    begin(w,WOLFSSH_FTP_EXTENDED);putstr(w,"fsync@openssh.com",17);putstr(w,handle,length);
    return exchange(w,WOLFSSH_FTP_STATUS);
}
