/* Real C16 engine acceptance. Test-only anchors and endpoint; no account data. */
#include "../internal.h"
static const TgByte password[]={97,32,110,97,195,175,118,101,32,116,101,115,116,32,112,97,115,115,119,111,114,100};
int main(void) {
    TgClient *c;unsigned round,sent,shown,i;unsigned long start,now;struct timeval wait;
    const char *message="Hello from the isolated C16 Telegram fixture";
    for(round=0;round<2;round++){
        c=tg_client_new("/etc/telegram.conf","/tmp/telegram.session");if(!c){puts("TG_FAILED: private config/session");return 1;}
        c->fixture_ip[0]=192;c->fixture_ip[1]=168;c->fixture_ip[2]=64;c->fixture_ip[3]=1;c->fixture_port=7100;
        sent=shown=0;rfl_poll_clock(&start);
        while(1){
            tg_client_step(c);
            if(c->state==TG_FAILED){printf("TG_FAILED: %s\n",c->status);tg_client_free(c);return 1;}
            if(c->state==TG_QR&&!shown){puts("TG_QR_READY");fflush(stdout);shown=1;}
            if(c->state==TG_PASSWORD){for(i=0;i<sizeof(password);i++)tg_client_password_key(c,password[i]);tg_client_password_key(c,10);}
            if(c->state==TG_READY){
                if(round){puts("TG_RESTORED");fflush(stdout);break;}
                if(!sent){if(!tg_client_send(c,(const TgByte *)message,strlen(message)))return 1;sent=1;}
                else{puts("TG_SENT");fflush(stdout);break;}
            }
            rfl_poll_clock(&now);if(now-start>120000UL){printf("TG_FAILED: timeout %s\n",c->status);tg_client_free(c);return 1;}
            wait.tv_sec=0;wait.tv_usec=5000;select(0,NULL,NULL,NULL,&wait);
        }
        tg_client_free(c);
    }
    puts("TG_GUEST_OK");return 0;
}
