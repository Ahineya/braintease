/* Exercise the generic framed worker through actual guest pipes and sockets. */
#include <netutil.h>
#include <sys/wait.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include "../ripple_http.h"
static unsigned checks,failures;
static int input,output;
static pid_t child;
static void check(int yes,const char *name) {
    checks++;if(!yes){failures++;fprintf(stderr,"FAIL: %s\n",name);}
}
static void start(void) {
    int in[2],out[2],fd;
    if(pipe(in)||pipe(out)){perror("pipe");exit(2);}
    child=fork();if(child<0)exit(2);
    if(!child) {
        if(dup2(in[0],0)<0||dup2(out[1],1)<0)_exit(126);
        for(fd=3;fd<FD_SETSIZE;fd++)close(fd);
        {char *args[]={"htget","--worker",NULL};execv("/bin/htget",args);}_exit(127);
    }
    close(in[0]);close(out[1]);input=in[1];output=out[0];
}
static void stop(int success) {
    int status;char byte;
    close(input);
    check(read(output,&byte,1)==0,"worker exits at request EOF");close(output);
    check(waitpid(child,&status,0)==child&&WIFEXITED(status)&&((WEXITSTATUS(status)==0)==success),"worker exit status");
}
static void send_request(const char *payload,unsigned length) {
    unsigned char prefix[2];unsigned i,n;
    prefix[0]=length>>8;prefix[1]=length&255;
    /* Split the length and payload across writes, including pipe capacity. */
    check(!nu_write(input,prefix,1,30000)&&!nu_write(input,prefix+1,1,30000),"request prefix");
    for(i=0;i<length;i+=n){n=length-i;if(n>173)n=173;if(nu_write(input,payload+i,n,30000)){check(0,"request payload");break;}}
}
static void fetch(const char *scheme,const char *path,const char *headers,int success,const char *expected) {
    char request[5500],data[16385],body[9000];unsigned char prefix[3];
    unsigned size,kind,heads=0,used=0,done=0,i;int status;
    snprintf(request,sizeof(request),"%s://tls.test%s/%s\n%s\n",!strcmp(scheme,"https12")?"https":scheme,!strcmp(scheme,"https12")?":444":"",path,headers);
    fprintf(stderr,"WORKER_REQUEST %s://tls.test/%s\n",scheme,path);
    send_request(request,strlen(request));
    while(!done) {
        if(nu_exact(output,prefix,3,30000)){check(0,"response record prefix");break;}
        kind=prefix[0];size=(unsigned)prefix[1]*256+prefix[2];
        if(size>16384||nu_exact(output,data,size,30000)){check(0,"response record length");break;}
        data[size]=0;
        if(kind==RIPPLE_HTTP_HEADERS){heads++;check((strstr(data,"HTTP/1.1 ")==data||strstr(data,"HTTP/1.0 ")==data)&&strstr(data,"\r\n\r\n")!=NULL,"HTTP headers record");}
        else if(kind==RIPPLE_HTTP_BODY){if(!success)continue;if(used+size<sizeof(body)){memcpy(body+used,data,size);used+=size;}else check(0,"body bound");}
        else if(kind==RIPPLE_HTTP_DONE){check(success&&size==0&&heads==1,"successful response terminator");done=1;}
        else if(kind==RIPPLE_HTTP_ERROR){check(!success&&size>0,"failed response terminator");done=1;}
        else {check(0,"response record type");break;}
    }
    body[used]=0;if(success&&expected)check(!strcmp(body,expected),"exact body without neighbouring response bytes");
    if(success&&(!strcmp(path,"binary")||!strcmp(path,"gzip-binary"))) {
        int valid=used==5120;
        for(i=0;i<used;i++)if((unsigned char)body[i]!=(i&255))valid=0;
        check(valid,"binary framed body preserves NUL and high octets across records");
    }
    check(waitpid(child,&status,WNOHANG)==0,"same worker remains alive after response");
}
int main(void) {
    char large[4200];unsigned i;
    signal(SIGPIPE,SIG_IGN);start();
    fetch("http","one","Cookie: first=yes\n",1,"ONE");
    fetch("http","two","",1,"TWO");
    fetch("http","chunked","",1,"ABCDEF");
    fetch("http","binary","",1,NULL);
    fetch("http","gzip-fixed","",1,"GZIP BODY");
    fetch("http","gzip-chunked","",1,"GZIP BODY");
    fetch("http","gzip-concat","",1,"GZIP BODY");
    fetch("http","gzip-binary","",1,NULL);
    fetch("http","gzip-bad-crc","",0,NULL);
    fetch("http","after-bad","",1,"RECOVERED");
    fetch("http","gzip-short","",0,NULL);
    fetch("http","gzip-limit","",0,NULL);
    fetch("http","after-bad","",1,"RECOVERED");
    fetch("http","redirect","",1,"");
    fetch("http","after","",1,"AFTER");
    fetch("http","stale","",1,"STALE");
    fetch("http","retry","",1,"RETRY");
    fetch("http","bad","",0,NULL);
    fetch("http","after-bad","",1,"RECOVERED");
    fetch("http","empty","",1,"");
    fetch("http","notmodified","",1,"");
    fetch("http","close","",1,"CLOSE");
    fetch("http","old","",1,"OLD");
    fetch("http","partial","",0,NULL);
    fetch("http","after-bad","",1,"RECOVERED");
    /* Exercise multi-kilobyte requests with independently split writes. */
    strcpy(large,"X-A: ");for(i=5;i<1024;i++)large[i]='a';large[1024]='\n';
    memcpy(large+1025,large,1025);memcpy(large+2050,large,1025);memcpy(large+3075,large,1025);large[4100]=0;
    fetch("http","long",large,1,"LONG");
    fetch("https","one","",1,"TLS-ONE");
    fetch("https","two","",1,"TLS-TWO");
    fetch("https","chunked","",1,"TLS-CHUNKED");
    fetch("https","gzip-fixed","",1,"GZIP BODY");
    fetch("https","gzip-chunked","",1,"GZIP BODY");
    fetch("https","stale","",1,"TLS-STALE");
    fetch("https","retry","",1,"TLS-RETRY");
    fetch("https","binary","",1,NULL);
    fetch("https12","one","",1,"TLS-ONE");
    fetch("https12","two","",1,"TLS-TWO");
    stop(1);
    start();send_request("http://tls.test/one\nHost: wrong.test\n\n",strlen("http://tls.test/one\nHost: wrong.test\n\n"));stop(0);
    start();send_request("http://tls.test/one\n\0\n",22);stop(0);
    start();{unsigned char length[2]={255,255};nu_write(input,length,2,30000);}stop(0);
    printf("HTTP_WORKER checks=%u failures=%u\n",checks,failures);return failures?1:0;
}
