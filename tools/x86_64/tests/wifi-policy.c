#include <assert.h>
#include <stdio.h>
#include "../userland/wifi-policy.h"
int main(void) {
    WifiRecovery p={0};unsigned delays[]={2,4,8,16,32,60,60};
    for(unsigned i=0;i<7;i++) {
        uint32_t now=p.next;
        assert(wifi_recovery_step(&p,now,WIFI_STATE_DOWN,0)==WIFI_CONNECT);
        wifi_recovery_failed(&p,now);assert(p.delay==delays[i]);
        assert(wifi_recovery_step(&p,p.next-1,WIFI_STATE_DOWN,0)==WIFI_WAIT);
    }
    assert(wifi_recovery_step(&p,p.next,WIFI_STATE_UP,0)==WIFI_ONLINE && !p.delay);
    assert(wifi_recovery_step(&p,p.next,WIFI_STATE_UP,0)==WIFI_WAIT);
    assert(wifi_recovery_step(&p,p.next,WIFI_STATE_UP,1)==WIFI_STOP);
    assert(wifi_recovery_step(&p,p.next,WIFI_STATE_DOWN,1)==WIFI_WAIT);
    assert(wifi_recovery_step(&p,p.next,WIFI_STATE_DOWN,0)==WIFI_CONNECT);
    for(unsigned s=0;s<=3;s+=3)assert(wifi_recovery_step(&p,p.next,s,0)==WIFI_WAIT);
    p.next=0xfffffff0U;assert(wifi_recovery_step(&p,p.next,WIFI_STATE_DOWN,0)==WIFI_CONNECT);
    wifi_recovery_failed(&p,p.next);
    assert(wifi_recovery_step(&p,100,WIFI_STATE_DOWN,0)==WIFI_WAIT);
    assert(wifi_recovery_step(&p,p.next,WIFI_STATE_DOWN,0)==WIFI_CONNECT);
    puts("WiFi recovery policy passed: backoff, cap, pause, recovery, absent/unsafe refusal, clock wrap");
}
