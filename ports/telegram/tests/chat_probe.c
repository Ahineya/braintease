/* Exercise the same chat model on both real C16 guest execution paths. */
#include "../internal.h"
static const TgByte password[]={97,32,110,97,195,175,118,101,32,116,101,115,116,32,112,97,115,115,119,111,114,100};
static int visible(TgClient *c,const char *needle){unsigned i,n=tg_client_history_lines(c,60);for(i=0;i<n;i++)if(strstr(tg_client_history_line(c,i,60),needle))return 1;return 0;}
int main(void){TgClient *c;unsigned round,shown,browse,opened,announced,sent,i;unsigned long start,now;struct timeval wait;
    const char *text="Hello from the isolated C16 Telegram fixture";
    for(round=0;round<2;round++){
        c=tg_client_new("/etc/telegram.conf","/tmp/telegram.session");if(!c)return 1;shown=browse=opened=announced=sent=0;rfl_poll_clock(&start);
        while(1){
            tg_client_step(c);if(c->state==TG_FAILED){printf("TG_FAILED: %s\n",c->status);return 1;}
            if(c->state==TG_QR&&!shown){puts("TG_QR_READY");fflush(stdout);shown=1;}
            if(c->state==TG_PASSWORD){for(i=0;i<sizeof(password);i++)tg_client_password_key(c,password[i]);tg_client_password_key(c,10);}
            if(c->state==TG_READY&&!browse)browse=tg_client_browse(c);
            if(browse&&tg_client_chat_count(c)==5){
                if(round&&visible(c,"Existing Saved Messages history")){puts("TG_RESTORED");fflush(stdout);break;}
                if(!round&&!opened){
                    tg_client_filter(c,"bOb");if(tg_client_chat_count(c)!=1||strcmp(tg_client_chat_title(c,0),"Bob"))return 2;
                    if(!tg_client_open(c,0))return 3;tg_client_filter(c,"");opened=1;
                }
            }
            if(opened&&!announced&&visible(c,"Bob history")){puts("TG_CHAT_BOB_READY");fflush(stdout);announced=1;}
            if(announced&&!sent&&visible(c,"Live incoming fixture text")&&c->state==TG_READY&&!c->request){if(!tg_client_send_active(c,(const TgByte *)text,strlen(text)))return 4;sent=1;}
            if(sent&&c->state==TG_READY&&!c->outbox_pending&&visible(c,text)){puts("TG_SENT");fflush(stdout);break;}
            rfl_poll_clock(&now);if(now-start>120000UL){printf("TG_FAILED: timeout %s\n",c->status);return 5;}
            wait.tv_sec=0;wait.tv_usec=5000;select(0,NULL,NULL,NULL,&wait);
        }tg_client_free(c);
    }puts("TG_GUEST_OK");return 0;
}
