/* SPDX-License-Identifier: MIT */
#include "service.h"
int main(int argc,char **argv){
    struct sockaddr_un address;char command[SV_INPUT],response[512];char prefix[3];unsigned prefix_used=0,j;int fd,n,at=0,total,failed=1;fd_set reads,writes;struct timeval timeout;
    if(argc<4||argc>5||strcmp(argv[1],"--runtime")||(argc==5&&!sv_name(argv[4])))goto usage;
    if(strcmp(argv[3],"status")&&strcmp(argv[3],"start")&&strcmp(argv[3],"stop")&&strcmp(argv[3],"restart")&&strcmp(argv[3],"shutdown"))goto usage;
    memset(&address,0,sizeof(address));address.sun_family=AF_UNIX;
    if(sv_runtime(argv[2],0,address.sun_path,sizeof(address.sun_path))){perror("svc runtime");return 1;}
    signal(SIGPIPE,SIG_IGN);
    fd=socket(AF_UNIX,SOCK_STREAM,0);if(fd<0){perror("svc socket");return 1;}
    if(connect(fd,(struct sockaddr *)&address,sizeof(address))){perror("svc connect");close(fd);return 1;}
    total=snprintf(command,sizeof(command),"%s%s%s\n",argv[3],argc==5?" ":"",argc==5?argv[4]:"");
    while(at<total){n=write(fd,command+at,total-at);if(n>0)at+=n;else if(n<0&&errno==EINTR)continue;else goto done;}
    at=0;
    for(;;){
        FD_ZERO(&reads);FD_ZERO(&writes);FD_SET(fd,&reads);timeout.tv_sec=5;timeout.tv_usec=0;
        n=select(fd+1,&reads,&writes,NULL,&timeout);if(n<0&&errno==EINTR)continue;if(n<=0){fputs("svc: supervisor response timed out\n",stderr);failed=1;break;}
        n=read(fd,response,sizeof(response));if(!n)break;
        if(n<0){if(errno==EINTR)continue;failed=1;break;}
        for(j=0;j<(unsigned)n&&prefix_used<3;j++)prefix[prefix_used++]=response[j];
        if(prefix_used==3)failed=memcmp(prefix,"OK",2)||(prefix[2]!=' '&&prefix[2]!='\n');fwrite(response,1,n,stdout);
    }
done:
    close(fd);return failed;
usage:
    fputs("usage: svc --runtime PRIVATE_DIR status [NAME]\n       svc --runtime PRIVATE_DIR start|stop|restart NAME\n       svc --runtime PRIVATE_DIR shutdown\n",stderr);return 2;
}
