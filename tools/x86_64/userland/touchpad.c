#include <stdio.h>
#include <string.h>
extern long call(unsigned,long,long,long,long);
int main(int argc,char **argv) {
    if(argc!=2 || strcmp(argv[1],"status")){fputs("usage: touchpad status\n",stderr);return 2;}
    unsigned long state=call(0x7848,0,0,0,0);
    if(state>2){fputs("touchpad: control unavailable\n",stderr);return 1;}
    printf("Touchpad %s; reports %lu; malformed %lu; I2C abort %lu; pointer %lu,%lu\n",
        state==1?"ready":state==2?"stopped":"unavailable",
        (unsigned long)call(0x7848,1,0,0,0),(unsigned long)call(0x7848,2,0,0,0),
        (unsigned long)call(0x7848,3,0,0,0),(unsigned long)call(0x7848,4,0,0,0),
        (unsigned long)call(0x7848,5,0,0,0));
    if(state==2){char reason[160];if(call(0x7848,6,0,(long)reason,sizeof(reason))>=0)fprintf(stderr,"touchpad: %s\n",reason);}
    printf("BAR before D0 %08lx:%08lx; after D0 %08lx:%08lx\n",
        (unsigned long)call(0x7848,8,0,0,0),(unsigned long)call(0x7848,7,0,0,0),
        (unsigned long)call(0x7848,10,0,0,0),(unsigned long)call(0x7848,9,0,0,0));
    return state==1?0:1;
}
