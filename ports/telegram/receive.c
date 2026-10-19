/* SPDX-License-Identifier: MIT */
#include "internal.h"
static int resend(TgClient *c) {
    int n;TgU64 id;if(!c->request||c->retries++>=3)return 0;id=tg_message_id(c);if(!id)return 0;
    n=tg_encrypt(c->auth,c->salt,c->session,id,c->last_seq,c->last_query,c->last_n,c->unpacked,TG_MAX,0);
    if(n<0||!tg_queue(c,c->unpacked,n))return 0;c->request_id=id;c->deadline=c->now+60000UL;return 1;
}
static int ack(TgClient *c,TgU64 id) {
    TgTl t;unsigned i;c->acks[c->ack_n++]=id;if(c->ack_n<64)return 1;
    tg_tl_init(&t,c->scratch,TG_MAX);tg_w32(&t,0x62d6b459UL);tg_w32(&t,0x1cb5c415UL);tg_w32(&t,c->ack_n);
    for(i=0;i<c->ack_n;i++)tg_w64(&t,c->acks[i]);c->ack_n=0;return t.ok&&tg_send_encrypted(c,c->scratch,t.at,0);
}
static int remember(TgClient *c,TgU64 id) {
    unsigned i,minimum=0;for(i=0;i<c->seen_n;i++){if(c->seen[i]==id)return 0;if(c->seen[i]<c->seen[minimum])minimum=i;}
    if(c->seen_n<64)c->seen[c->seen_n++]=id;else{if(id<c->seen[minimum])return 0;c->seen[minimum]=id;}return 1;
}
static int body(TgClient *c,TgByte *data,unsigned n,TgU64 id,TgU32 seq,unsigned depth) {
    TgTl t;TgU32 constructor,code,length;TgU64 reference;const TgByte *p;unsigned i,count;TgByte *expanded;int size,ok;
    if(depth>4||n<4||n%4||!(id&1))return 0;
    tg_tl_init(&t,data,n);constructor=tg_u32(&t);
    if(seq&1)if(!ack(c,id))return 0;
    if(!remember(c,id))return 1;
    if(constructor!=0xa7eff811UL){
        int64_t when=(int64_t)(id>>32),now=(int64_t)time(NULL)+c->time_offset;
        if(when<now-300||when>now+30)return 0;
    }
    if(constructor==0x73f1f8dcUL){
        count=tg_u32(&t);if(count>64)return 0;
        for(i=0;i<count;i++){
            TgU64 inner_id=tg_u64(&t);TgU32 inner_seq=tg_u32(&t);length=tg_u32(&t);
            if(length>TG_MAX||length%4||inner_id>=id)return 0;p=tg_raw(&t,length);
            if(!p||!body(c,(TgByte *)p,length,inner_id,inner_seq,depth+1))return 0;
            if(!c->socket||c->state==TG_FAILED)return 1;
        }
        return t.ok&&t.at==t.n;
    }
    if(constructor==0x3072cfa1UL){
        unsigned packed;p=tg_bytes(&t,&packed);if(!p||t.at!=t.n)return 0;
        expanded=malloc(TG_MAX);if(!expanded)return 0;size=tg_gunzip(p,packed,expanded,TG_MAX);
        /* Already authenticated/remembered the wrapper. The contained body is
         * a representation of this same message, not a second replay ID. */
        if(c->seen_n) {for(i=0;i<c->seen_n;i++)if(c->seen[i]==id){c->seen[i]=c->seen[--c->seen_n];break;}}
        ok=size>=4&&body(c,expanded,size,id,0,depth+1);free(expanded);return ok;
    }
    if(constructor==0xf35c6d01UL){
        unsigned kind;reference=tg_u64(&t);if(!t.ok)return 0;if(reference!=c->request_id||!c->request)return 1;
        kind=c->request;c->request=0;return tg_account_result(c,data+t.at,n-t.at,kind);
    }
    if(constructor==0xedab447bUL){
        reference=tg_u64(&t);tg_u32(&t);code=tg_u32(&t);p=tg_raw(&t,8);
        if(!p||t.at!=t.n||code!=48)return 0;memcpy(c->salt,p,8);
        /* A salt correction can refer to an acknowledgement, not our RPC.
         * Repeated server updates will be acknowledged using the new salt. */
        return c->request&&reference==c->request_id?resend(c):1;
    }
    if(constructor==0xa7eff811UL){
        reference=tg_u64(&t);tg_u32(&t);code=tg_u32(&t);
        if(!t.ok||t.at!=t.n||reference!=c->request_id||(code!=16&&code!=17))return 0;
        c->time_offset=(int32_t)(id>>32)-(int32_t)time(NULL);c->last_id=0;return resend(c);
    }
    if(constructor==0x9ec20908UL){
        tg_u64(&t);tg_u64(&t);p=tg_raw(&t,8);if(!p||t.at!=t.n)return 0;memcpy(c->salt,p,8);return 1;
    }
    if(c->authorized&&c->model&&(constructor==TL_updates||constructor==TL_updatesCombined||constructor==TL_updateShort||constructor==TL_updateShortMessage||constructor==TL_updateShortChatMessage||constructor==TL_updatesTooLong))return tg_model_update(c,data,n);
    if(constructor==TL_updateShort)constructor=tg_u32(&t);
    else if(constructor==TL_updates||constructor==TL_updatesCombined){
        if(tg_u32(&t)!=0x1cb5c415UL)return 0;count=tg_u32(&t);if(!count)return 1;constructor=tg_u32(&t);
    }
    if(constructor==TL_updateLoginToken&&!c->authorized&&
        (c->state==TG_QR||c->request==TG_REQ_EXPORT||c->request==TG_REQ_IMPORT))c->login_signal=1;
    /* Other service notifications and account updates are intentionally ignored
     * in this login/Saved Messages milestone, after transport authentication. */
    return 1;
}
int tg_receive(TgClient *c,TgByte *packet,unsigned n) {
    unsigned i;TgTl t;TgU64 id;TgU32 seq;int length;
    if(n==4&&tg_le32(packet)==0xfffffe6cUL&&c->has_key&&c->key_restarts++==0){
        c->has_key=c->authorized=c->outbox_pending=0;c->user_id=0;tg_model_free(c);
        tg_erase(c->auth,256);tg_erase(c->outbox,sizeof(c->outbox));return tg_connect_dc(c);
    }
    if(!c->has_key){
        if(n<24)return 0;for(i=0;i<8;i++)if(packet[i])return 0;
        id=tg_le64(packet+8);if(!(id&1)||tg_le32(packet+16)!=n-20)return 0;
        return tg_handshake_receive(c,packet+20,n-20);
    }
    length=tg_decrypt(c->auth,packet,n,8);if(length<4){tg_fail(c,"Telegram packet encryption or MAC check failed");return 0;}
    if(!tg_equal(packet+32,c->session,8)){tg_fail(c,"Telegram packet session ID mismatch");return 0;}
    tg_tl_init(&t,packet+40,16);id=tg_u64(&t);seq=tg_u32(&t);tg_u32(&t);
    /* Incoming authenticated packets may carry a different server salt.
     * Like TDLib SessionConnection::on_raw_packet, validate MAC, session,
     * timestamp and replay IDs rather than equating this to our outgoing
     * salt. Only authenticated new_session/bad_server_salt change that salt. */
    if(!body(c,packet+56,length,id,seq,0)){if(c->state!=TG_FAILED){char error[128];snprintf(error,sizeof(error),"Invalid Telegram message %08lx (request %u)",(unsigned long)tg_le32(packet+56),c->request);tg_fail(c,error);}return 0;}
    if(c->ack_n&&c->socket){
        tg_tl_init(&t,c->scratch,TG_MAX);tg_w32(&t,0x62d6b459UL);tg_w32(&t,0x1cb5c415UL);tg_w32(&t,c->ack_n);
        for(i=0;i<c->ack_n;i++)tg_w64(&t,c->acks[i]);c->ack_n=0;return t.ok&&tg_send_encrypted(c,c->scratch,t.at,0);
    }
    return 1;
}
