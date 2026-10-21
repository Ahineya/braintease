/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "keys.h"
#include <stdio.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
int main(int argc,char **argv) {
    char value[8];int r,saved,child=-1,parent=getpid();(void)argv;
    if(argc>1) {
        child=fork();if(child<0)return 2;
        if(!child){usleep(500000UL);kill(parent,SIGQUIT);_exit(0);}
    }
    r=ssh_secret("Secret: ",value,sizeof(value));saved=errno;
    if(child>0)while(waitpid(child,NULL,0)<0&&errno==EINTR);
    ssh_wipe(value,sizeof(value));
    puts(!r?"SECRET_OK":saved==EOVERFLOW?"SECRET_OVERFLOW":saved==EINTR?"SECRET_INTERRUPTED":"SECRET_ERROR");
    return r?1:0;
}
