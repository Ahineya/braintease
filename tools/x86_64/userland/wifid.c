/* One supervised recovery process; native startup never runs from a NIC poll.
 * Protocol/key handling remains native for this transition. Full reconnect is
 * disruptive; it is not in-place rekey. IP/remote-service policy is an explicit
 * administrator-owned online hook, not hardcoded supervisor manipulation. */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <errno.h>
#include "wifi-client.h"
#include "wifi-policy.h"
static volatile int stopping;
static void caught(int n){stopping=n;signal(n,caught);}
static uint32_t now_ms(void){struct timespec t;if(clock_gettime(CLOCK_MONOTONIC,&t))return 0;return (uint32_t)t.tv_sec*1000+(uint32_t)t.tv_nsec/1000000;}
static int online_hook(void) {
    int status,pid=fork();
    if(pid<0)return -1;
    if(!pid){char *args[]={"sh","/etc/wifi-online.sh",0};char *env[]={"PATH=/usr/bin:/bin","HOME=/root",0};execve("/bin/sh",args,env);_exit(127);}
    while(waitpid(pid,&status,0)<0){if(stopping || errno!=EINTR)return -1;}
    return WIFEXITED(status)&&!WEXITSTATUS(status)?0:-1;
}
int main(void) {
    WifiRecovery policy={0};WifiSnapshot r;unsigned previous=99,status,action;unsigned long generation=0;
    if(geteuid()){fputs("wifid: root is required\n",stderr);return 1;}
    signal(SIGTERM,caught);signal(SIGINT,caught);
    policy.next=now_ms();
    puts("wifid: recovery enabled; retry delay2..60s; wifi down pauses recovery");fflush(stdout);
    while(!stopping) {
        status=wifi_request(WIFI_CONTROL_STATUS,&r);
        if(status){fprintf(stderr,"wifid: unsupported/rejected control status%u\n",status);return 1;}
        if(r.generation!=generation){policy.online=0;generation=r.generation;}
        if(r.state!=previous){printf("wifid: state%u %s\n",r.state,r.reason);fflush(stdout);previous=r.state;}
        action=wifi_recovery_step(&policy,now_ms(),r.state,access(WIFI_PAUSE_FILE,F_OK)==0);
        if(action==WIFI_CONNECT) {
            /* Recheck administrative pause immediately before and after the
             * synchronous native operation. A concurrent down wins. */
            if(access(WIFI_PAUSE_FILE,F_OK)==0)continue;
            puts("wifid: reconnecting");fflush(stdout);
            status=wifi_request(WIFI_CONTROL_RECONNECT,&r);
            if(stopping || access(WIFI_PAUSE_FILE,F_OK)==0)wifi_request(WIFI_CONTROL_DOWN,&r);
            if(status || r.outcome || r.state!=WIFI_STATE_UP) {
                wifi_recovery_failed(&policy,now_ms());
                fprintf(stderr,"wifid: reconnect failed status%u: %s; retry in%us\n",status,r.reason,policy.delay);
            }
        } else if(action==WIFI_STOP)wifi_request(WIFI_CONTROL_DOWN,&r);
        else if(action==WIFI_ONLINE) {
            if(online_hook()) {
                fputs("wifid: online hook failed; retry in5s\n",stderr);
                sleep(5);policy.online=0;
            } else {puts("WIFI_ONLINE address configured; service start requests sent");fflush(stdout);}
        }
        if(!stopping)sleep(1);
    }
    wifi_request(WIFI_CONTROL_DOWN,&r);puts("wifid: stopped");return 0;
}
