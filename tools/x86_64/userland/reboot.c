/* Arm reset; existing orderly shutdown stops services and syncs/quiesces all
 * devices before runtime returns. Failed exec cancels the pending reset. */
#include <stdio.h>
#include <unistd.h>
#include <mmio_constants.h>
static int request(unsigned action) {
    volatile unsigned short d[4];unsigned long p=(unsigned long)d;
    d[0]=1;d[1]=4;d[2]=action;d[3]=0;
    *(volatile unsigned short *)(unsigned long)MMIO_DEVICE_DESC_BANK=p>>16;
    *(volatile unsigned short *)(unsigned long)MMIO_DEVICE_DESC_OFF=p;
    *(volatile unsigned short *)(unsigned long)MMIO_DEVICE_COMMAND=0x105;
    return *(volatile unsigned short *)(unsigned long)MMIO_DEVICE_STATUS?0:d[3];
}
int main(int argc,char **argv) {
    (void)argv;
    if(argc!=1 || geteuid()){fputs("reboot: root required; no arguments\n",stderr);return 1;}
    if(!request(1)){fputs("reboot: firmware reset unavailable\n",stderr);return 1;}
    puts("reboot: requesting orderly shutdown");fflush(stdout);
    char *args[]={"sh","-c","shutdown",0};char *env[]={"PATH=/usr/bin:/bin","HOME=/root",0};
    execve("/bin/sh",args,env);
    request(2);perror("reboot shutdown shell");return 1;
}
