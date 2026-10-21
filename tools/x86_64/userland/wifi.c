/* Local development control for the embedded test profile. Key protocol code
 * remains native; wifid owns retry policy. No compiler inline asm workaround
 * was applied before recording test_native_inline_asm_continuation RED. */
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include "wifi-client.h"
int main(int argc,char **argv) {
    unsigned operation,status;int fd;WifiSnapshot r;
    if(argc!=2){fputs("usage: wifi status|down|reconnect\n",stderr);return 2;}
    if(!strcmp(argv[1],"status"))operation=WIFI_CONTROL_STATUS;
    else if(!strcmp(argv[1],"down"))operation=WIFI_CONTROL_DOWN;
    else if(!strcmp(argv[1],"reconnect"))operation=WIFI_CONTROL_RECONNECT;
    else {fputs("usage: wifi status|down|reconnect\n",stderr);return 2;}
    if(operation && geteuid()){fputs("wifi: root is required\n",stderr);return 1;}
    if(operation==WIFI_CONTROL_DOWN) {
        fd=open(WIFI_PAUSE_FILE,O_WRONLY|O_CREAT|O_TRUNC,0600);
        if(fd<0){perror("wifi pause");return 1;}close(fd);
    } else if(operation==WIFI_CONTROL_RECONNECT) {
        if(unlink(WIFI_PAUSE_FILE)<0 && errno!=ENOENT){perror("wifi resume");return 1;}
    }
    status=wifi_request(operation,&r);
    if(status){fprintf(stderr,"wifi: radio control unsupported or rejected (status %u)\n",status);return 1;}
    printf("WiFi %s; reconnects %lu; attempts %lu; recovery %s\n",
           r.state==WIFI_STATE_UP?"up":r.state==WIFI_STATE_DOWN?"down":r.state==WIFI_STATE_BLOCKED?"blocked (DMA stop unconfirmed)":"unavailable",
           r.generation,r.attempts,access(WIFI_PAUSE_FILE,F_OK)==0?"paused":"enabled");
    if(r.reason[0])fprintf(stderr,"wifi: %s\n",r.reason);
    if(r.outcome)return 1;
    if(operation==WIFI_CONTROL_RECONNECT)puts("Radio ready; wifid restores configured IP and remote services.");
    return 0;
}
