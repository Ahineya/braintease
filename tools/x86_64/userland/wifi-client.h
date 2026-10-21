/* Private x64 control ABI; checked logical MMIO (see inline-asm RED rct). */
#ifndef RIPLIX_WIFI_CLIENT_H
#define RIPLIX_WIFI_CLIENT_H
#ifndef WIFI_CLIENT_TEST
#include <mmio_constants.h>
#endif
#include "../boot/wifi-control.h"
#define WIFI_PAUSE_FILE "/var/run/wifi.paused"
typedef struct {
    unsigned state, outcome;
    unsigned long generation, attempts;
    char reason[WIFI_CONTROL_TEXT];
} WifiSnapshot;
#ifdef WIFI_CLIENT_TEST
unsigned wifi_request(unsigned,WifiSnapshot *);
#else
static unsigned wifi_request(unsigned operation, WifiSnapshot *out) {
    volatile unsigned short d[WIFI_CONTROL_CELLS];
    unsigned long address=(unsigned long)d;
    unsigned short i,status;
    for(i=0;i<WIFI_CONTROL_CELLS;i++)d[i]=0;
    d[0]=1;d[1]=WIFI_CONTROL_CELLS;d[3]=operation;
    *(volatile unsigned short *)(unsigned long)MMIO_DEVICE_DESC_BANK=(unsigned short)(address>>16);
    *(volatile unsigned short *)(unsigned long)MMIO_DEVICE_DESC_OFF=(unsigned short)address;
    *(volatile unsigned short *)(unsigned long)MMIO_DEVICE_COMMAND=WIFI_CONTROL_COMMAND;
    status=*(volatile unsigned short *)(unsigned long)MMIO_DEVICE_STATUS;
    if(status)return status;
    if(d[6]>WIFI_STATE_BLOCKED || d[7]>WIFI_RESULT_BUSY)return 5;
    out->state=d[6];out->outcome=d[7];
    out->generation=(unsigned long)d[8]|((unsigned long)d[9]<<16);
    out->attempts=(unsigned long)d[10]|((unsigned long)d[11]<<16);
    for(i=0;i<WIFI_CONTROL_TEXT;i++) {
        if(d[12+i]>127)return 5;
        out->reason[i]=(char)d[12+i];
    }
    out->reason[WIFI_CONTROL_TEXT-1]=0;return 0;
}
#endif
#endif
