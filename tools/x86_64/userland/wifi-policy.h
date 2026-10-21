/* Retry policy is userland-only. All deadlines use wrapping32-bit milliseconds. */
#ifndef RIPLIX_WIFI_POLICY_H
#define RIPLIX_WIFI_POLICY_H
#include <stdint.h>
#include "../boot/wifi-control.h"
typedef struct {uint32_t next;unsigned delay,online;} WifiRecovery;
enum {WIFI_WAIT,WIFI_CONNECT,WIFI_ONLINE,WIFI_STOP};
static unsigned wifi_recovery_step(WifiRecovery *p,uint32_t now,unsigned state,unsigned paused) {
    if(paused){p->online=0;p->delay=0;p->next=now;return state==WIFI_STATE_UP?WIFI_STOP:WIFI_WAIT;}
    if(state==WIFI_STATE_UP){
        p->delay=0;p->next=now;
        if(!p->online){p->online=1;return WIFI_ONLINE;}
        return WIFI_WAIT;
    }
    p->online=0;
    if(state!=WIFI_STATE_DOWN || (uint32_t)(now-p->next)>=0x80000000U)return WIFI_WAIT;
    return WIFI_CONNECT;
}
static void wifi_recovery_failed(WifiRecovery *p,uint32_t now) {
    p->delay=p->delay?(p->delay>=30?60:p->delay*2):2;
    p->next=now+(uint32_t)p->delay*1000;
}
#endif
