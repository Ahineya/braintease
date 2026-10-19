/* SPDX-License-Identifier: MIT */
#include "internal.h"
static const TgByte production[5][4]={{149,154,175,50},{149,154,167,51},{149,154,175,100},{149,154,167,91},{149,154,171,5}};
static const TgByte testing[3][4]={{149,154,175,10},{149,154,167,40},{149,154,175,117}};
void tg_state(TgClient *c,unsigned state,const char *text) {c->state=state;snprintf(c->status,sizeof(c->status),"%s",text);c->revision++;}
void tg_transport_close(TgClient *c) {
    if(c->connecting){rfl_poll_drop(c->connecting);c->connecting=NULL;}
    if(c->socket){rfl_socket_release(c->socket);c->socket=NULL;}
    c->tx_n=c->tx_at=c->rx_n=0;tg_handshake_free(c);
}
void tg_fail(TgClient *c,const char *text) {
    tg_state(c,TG_FAILED,text);tg_transport_close(c);tg_erase(c->password,sizeof(c->password));c->password_n=0;c->qr[0]=0;
}
TgU64 tg_message_id(TgClient *c) {
    struct timespec now;TgU64 id;
    if(clock_gettime(CLOCK_REALTIME,&now)<0)return 0;
    id=((TgU64)(TgU32)(now.tv_sec+c->time_offset)<<32)|((((TgU64)(unsigned long)now.tv_nsec<<32)/1000000000UL)&0xfffffffcUL)|4;
    if(id<=c->last_id)id=c->last_id+4;c->last_id=id;return id;
}
int tg_queue(TgClient *c,const TgByte *p,unsigned n) {
    if(n>TG_MAX||n%4)return 0;
    if(c->tx_at){memmove(c->tx,c->tx+c->tx_at,c->tx_n-c->tx_at);c->tx_n-=c->tx_at;c->tx_at=0;}
    if(n+4>sizeof(c->tx)-c->tx_n)return 0;
    tg_put32(c->tx+c->tx_n,n);memcpy(c->tx+c->tx_n+4,p,n);c->tx_n+=n+4;return 1;
}
int tg_send_plain(TgClient *c,const TgByte *p,unsigned n) {
    TgByte *out=c->unpacked;TgU64 id=tg_message_id(c);if(!id||n>TG_MAX-20)return 0;
    memset(out,0,8);tg_put64(out+8,id);tg_put32(out+16,n);memcpy(out+20,p,n);c->deadline=c->now+30000UL;
    return tg_queue(c,out,n+20);
}
int tg_send_encrypted(TgClient *c,const TgByte *p,unsigned n,int content) {
    TgU64 id=tg_message_id(c);TgU32 seq=c->sequence*2+(content?1:0);int size;if(!id)return 0;
    size=tg_encrypt(c->auth,c->salt,c->session,id,seq,p,n,c->unpacked,TG_MAX,0);
    if(size<0||!tg_queue(c,c->unpacked,size))return 0;
    if(content){c->sequence++;c->request_id=id;c->last_seq=seq;c->deadline=c->now+60000UL;}return 1;
}
int tg_connect_dc(TgClient *c) {
    const TgByte *ip;unsigned port=443;
    if(c->dc<1||c->dc>(c->test?3:5))return 0;
    ip=c->test?testing[c->dc-1]:production[c->dc-1];
#ifdef TG_TESTING
    if(c->fixture_port){ip=c->fixture_ip;port=c->fixture_port;}
#endif
    tg_transport_close(c);c->initialized=0;c->request=0;c->phase=0;c->sequence=0;c->seen_n=c->seen_at=c->ack_n=0;c->last_id=0;
    tg_model_reconnect(c);
    if(!tg_random(c->session,8))return 0;
    c->connecting=rfl_poll_connect(c->poll,ip,port);if(!c->connecting||rfl_poll_timeout(c->connecting,15000UL)<0)return 0;
    c->deadline=c->now+30000UL;tg_state(c,TG_CONNECTING,"Connecting to Telegram");return 1;
}
TgClient *tg_client_new(const char *config,const char *session) {
    TgClient *c;if(!config||!session||strlen(session)>255)return NULL;
    c=calloc(1,sizeof(*c));if(!c)return NULL;c->dc=2;strcpy(c->session_path,session);
#ifdef TG_TESTING
    c->fixture_ip[0]=192;c->fixture_ip[1]=168;c->fixture_ip[2]=64;c->fixture_ip[3]=1;c->fixture_port=7100;
#endif
    if(!tg_config(c,config)||!tg_load(c)||(c->poll=rfl_poll_create())==NULL||(c->driver=rfl_poll_driver(c->poll))==NULL){tg_client_free(c);return NULL;}
    tg_state(c,TG_CONNECTING,"Starting guest Telegram client");return c;
}
void tg_client_free(TgClient *c) {
    if(!c)return;tg_transport_close(c);tg_model_free(c);if(c->driver)rfl_poll_driver_drop(c->driver);if(c->poll)rfl_poll_release(c->poll);tg_erase(c,sizeof(*c));free(c);
}
unsigned tg_client_state(const TgClient *c){return c->state;}
unsigned long tg_client_revision(const TgClient *c){return c->revision;}
const char *tg_client_status(const TgClient *c){return c->status;}
const char *tg_client_qr(const TgClient *c){return c->qr;}
const char *tg_client_hint(const TgClient *c){return c->hint;}
unsigned tg_client_password_length(const TgClient *c){return c->password_n;}
int tg_client_restart(TgClient *c) {if(c->state!=TG_FAILED)return 0;return tg_connect_dc(c);}
static int expired(unsigned long now,unsigned long deadline){return (TgU32)(now-deadline)<0x80000000UL;}
void tg_client_step(TgClient *c) {
    long n;int status;unsigned budget;TgU32 frame;unsigned long now;unsigned short events[33];TgByte capabilities[3];
    if(c->state==TG_FAILED)return;
    if(!c->crypto_checked){
        if(rfl_crypto_call(0,1,NULL,0,NULL,0,NULL,0,capabilities,3)!=3||capabilities[0]!=1||
            (((capabilities[1]|(unsigned)capabilities[2]<<8)&0x19f0U)!=0x19f0U)){
            tg_fail(c,"Crypto device lacks required MTProto operations; rebuild the VM");return;
        }
        c->crypto_checked=1;
    }
    if(rfl_poll_clock(&now)<0){tg_fail(c,"Guest clock unavailable");return;}c->now=now;
    if(!c->socket&&!c->connecting){if(!tg_connect_dc(c))tg_fail(c,"Could not start guest connection");return;}
    if(c->connecting){
        status=rfl_poll_status(c->connecting,now);
        if(!status){if(rfl_poll_turn(c->poll,0,&now,events)<0){tg_fail(c,"Guest networking reactor failed");return;}
            c->now=now;status=rfl_poll_status(c->connecting,now);}
        if(!status)return;
        if(status!=1){tg_fail(c,"Telegram connection failed or timed out");return;}
        c->socket=rfl_poll_connect_result(c->connecting);rfl_poll_drop(c->connecting);c->connecting=NULL;
        if(!c->socket){tg_fail(c,"Could not acquire connected socket");return;}
        memset(c->tx,0xee,4);c->tx_n=4;
        if(!(c->has_key?tg_rpc(c,c->authorized?TG_REQ_SELF:(c->import_n?TG_REQ_IMPORT:TG_REQ_EXPORT)):tg_handshake_start(c))){tg_fail(c,"Could not initialize Telegram handshake");return;}
    }
    for(budget=0;budget<4&&c->tx_at<c->tx_n;budget++) {
        n=rfl_socket_write(c->socket,c->tx+c->tx_at,c->tx_n-c->tx_at);
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))break;
        if(n<=0){tg_fail(c,"Telegram connection closed while writing");return;}c->tx_at+=n;
    }
    if(c->tx_at==c->tx_n)c->tx_at=c->tx_n=0;
    for(budget=0;budget<4;budget++) {
        if(c->rx_n>=sizeof(c->rx)){tg_fail(c,"Telegram packet exceeds client limit");return;}
        n=rfl_socket_read(c->socket,c->rx+c->rx_n,sizeof(c->rx)-c->rx_n);
        if(n<0&&(errno==EAGAIN||errno==EWOULDBLOCK||errno==EINTR))break;
        if(n<=0){tg_fail(c,"Telegram connection closed");return;}c->rx_n+=n;
        while(c->rx_n>=4){
            frame=tg_le32(c->rx);if(frame>TG_MAX||frame<4||frame%4){tg_fail(c,"Invalid Telegram transport frame");return;}
            if(c->rx_n<frame+4)break;
            if(!tg_receive(c,c->rx+4,frame)){if(c->state!=TG_FAILED)tg_fail(c,"Telegram packet failed authentication or validation");return;}
            if(c->state==TG_FAILED)return;
            if(!c->socket)return; /* DC migration replaced the transport. */
            memmove(c->rx,c->rx+4+frame,c->rx_n-4-frame);c->rx_n-=4+frame;
        }
    }
    if(c->state==TG_QR&&!c->request&&(c->login_signal||expired(now,c->token_deadline))) {
        c->qr[0]=0;tg_state(c,TG_CONNECTING,c->login_signal?"Confirming QR login":"Refreshing expired login QR");c->login_signal=0;
        if(!tg_rpc(c,TG_REQ_EXPORT))tg_fail(c,"Could not refresh QR token");
    }else if((c->phase||c->request)&&expired(now,c->deadline))tg_fail(c,"Telegram request timed out; press r to reconnect");
    if(c->authorized&&c->outbox_pending&&!c->request&&(c->state==TG_READY||c->state==TG_SENDING)){
        if(!tg_rpc(c,TG_REQ_SEND)){tg_fail(c,"Could not send queued message");return;}
        tg_state(c,TG_SENDING,"Sending queued message");
    }
    tg_model_step(c);
}
