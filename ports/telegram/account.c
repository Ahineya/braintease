/* SPDX-License-Identifier: MIT */
#include "internal.h"
#include "../../runtime/include/ripple_unicode.h"
int tg_rpc(TgClient *c,unsigned kind) {
    TgTl t;if(c->request||!c->has_key)return 0;
    tg_tl_init(&t,c->last_query,sizeof(c->last_query));
    if(!c->initialized){
        tg_w32(&t,TL_invokeWithLayer);tg_w32(&t,TG_LAYER);tg_w32(&t,TL_initConnection);tg_w32(&t,0);tg_w32(&t,c->api_id);
        tg_wtext(&t,"C16 guest");tg_wtext(&t,"Fuse-X/FUZIX");tg_wtext(&t,"0.1");tg_wtext(&t,"en");tg_wtext(&t,"");tg_wtext(&t,"en");
    }
    if(kind==TG_REQ_EXPORT){tg_w32(&t,TL_auth_exportLoginToken);tg_w32(&t,c->api_id);tg_wtext(&t,c->api_hash);tg_w32(&t,0x1cb5c415UL);tg_w32(&t,0);}
    else if(kind==TG_REQ_IMPORT){tg_w32(&t,TL_auth_importLoginToken);tg_wbytes(&t,c->import_token,c->import_n);}
    else if(kind==TG_REQ_PASSWORD)tg_w32(&t,TL_account_getPassword);
    else if(kind==TG_REQ_SELF){tg_w32(&t,TL_users_getUsers);tg_w32(&t,0x1cb5c415UL);tg_w32(&t,1);tg_w32(&t,TL_inputUserSelf);}
    else if(kind==TG_REQ_SEND){
        tg_w32(&t,TL_messages_sendMessage);tg_w32(&t,2);tg_model_send_peer(c,&t);tg_wbytes(&t,c->outbox,c->outbox_n);tg_wraw(&t,c->random_id,8);
    }else if(!tg_model_query(c,&t,kind))return 0;
    if(!t.ok)return 0;c->last_n=t.at;c->request=kind;c->retries=0;
    if(!tg_send_encrypted(c,c->last_query,c->last_n,1)){c->request=0;return 0;}c->initialized=1;
    return 1;
}
static TgU64 user(TgTl *t) {
    if(tg_u32(t)!=TL_user){t->ok=0;return 0;}tg_u32(t);tg_u32(t);return tg_u64(t);
}
static int authorization(TgClient *c,TgTl *t) {
    TgU32 flags;TgU64 id;unsigned n;
    if(tg_u32(t)!=TL_auth_authorization)return 0;flags=tg_u32(t);
    if(flags&2)tg_u32(t);if(flags&1)tg_u32(t);if(flags&4)tg_bytes(t,&n);
    if(flags&~7UL)return 0;id=user(t);if(!t->ok||!id)return 0;
    c->user_id=id;c->authorized=1;c->import_n=0;c->qr[0]=0;tg_erase(c->import_token,sizeof(c->import_token));
    if(!tg_store(c)){tg_fail(c,"Logged in, but could not save private session file");return 1;}
    tg_state(c,TG_READY,"Logged in. Session saved. Send a message to Saved Messages.");return 1;
}
static void uri(TgClient *c,const TgByte *token,unsigned n) {
    static const char alphabet[]="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    unsigned i,at=17;TgU32 word;strcpy(c->qr,"tg://login?token=");
    /* The prefix has 17 octets including '='. */
    for(i=0;i<n;i+=3){word=(TgU32)token[i]<<16;if(i+1<n)word|=(TgU32)token[i+1]<<8;if(i+2<n)word|=token[i+2];
        c->qr[at++]=alphabet[(word>>18)&63];c->qr[at++]=alphabet[(word>>12)&63];
        if(i+1<n)c->qr[at++]=alphabet[(word>>6)&63];if(i+2<n)c->qr[at++]=alphabet[word&63];}
    c->qr[at]=0;
}
static int password_parameters(TgClient *c,TgTl *t) {
    TgU32 flags;const TgByte *p;unsigned n;
    flags=tg_u32(t);if(!(flags&4)||tg_u32(t)!=TL_passwordKdfAlgoSHA256SHA256PBKDF2HMACSHA512iter100000SHA256ModPow)return 0;
    p=tg_bytes(t,&n);if(!p||n>128)return 0;c->salt1_n=n;memcpy(c->salt1,p,n);
    p=tg_bytes(t,&n);if(!p||n>128)return 0;c->salt2_n=n;memcpy(c->salt2,p,n);
    c->g=tg_u32(t);p=tg_bytes(t,&n);if(!p||n!=256||!tg_group(p,c->g))return 0;
    p=tg_bytes(t,&n);if(!p||!n||n>256)return 0;memset(c->B,0,256);memcpy(c->B+256-n,p,n);c->srp_id=tg_u64(t);
    if(!t->ok||!tg_good_public(c->B,tg_prime))return 0;c->hint[0]=0;
    if(flags&8){p=tg_bytes(t,&n);if(!p)return 0;if(n>255)n=255;memcpy(c->hint,p,n);c->hint[n]=0;}
    tg_erase(c->password,sizeof(c->password));c->password_n=0;c->qr[0]=0;
    tg_state(c,TG_PASSWORD,"Enter your Telegram 2FA password");return 1;
}
static int rpc_error(TgClient *c,TgTl *t,unsigned kind) {
    const TgByte *p;TgU32 code;unsigned n,i;char error[128],message[160];
    code=tg_u32(t);p=tg_bytes(t,&n);if(!p||n>=sizeof(error)||t->at!=t->n)return 0;
    for(i=0;i<n;i++)error[i]=p[i]>=32&&p[i]<127?p[i]:'?';error[n]=0;
    if((kind==TG_REQ_EXPORT||kind==TG_REQ_IMPORT)&&!strcmp(error,"SESSION_PASSWORD_NEEDED"))return tg_rpc(c,TG_REQ_PASSWORD);
    if(kind==TG_REQ_CHECK&&(!strcmp(error,"PASSWORD_HASH_INVALID")||!strcmp(error,"SRP_ID_INVALID")))return tg_rpc(c,TG_REQ_PASSWORD);
    if(kind==TG_REQ_SELF&&code==401){c->authorized=0;c->outbox_pending=0;tg_model_free(c);return tg_rpc(c,TG_REQ_EXPORT);}
    if(kind==TG_REQ_SEND&&code!=401){c->outbox_pending=0;c->outbox_n=0;tg_erase(c->outbox,sizeof(c->outbox));snprintf(message,sizeof(message),"Send failed %lu: %s",(unsigned long)code,error);tg_state(c,TG_READY,message);return 1;}
    if(kind>=TG_REQ_STATE&&code!=401){tg_model_error(c,kind,error);snprintf(message,sizeof(message),"Telegram error %lu: %s",(unsigned long)code,error);tg_state(c,TG_READY,message);return 1;}
    snprintf(message,sizeof(message),"Telegram error %lu: %s",(unsigned long)code,error);tg_fail(c,message);return 1;
}
int tg_account_result(TgClient *c,TgByte *p,unsigned n,unsigned kind) {
    TgTl t;TgU32 constructor;TgByte *decoded;const TgByte *bytes;unsigned length;int size,ok;
    tg_tl_init(&t,p,n);constructor=tg_u32(&t);
    if(constructor==0x3072cfa1UL){
        bytes=tg_bytes(&t,&length);if(!bytes||t.at!=t.n)return 0;decoded=malloc(TG_MAX);if(!decoded)return 0;
        size=tg_gunzip(bytes,length,decoded,TG_MAX);if(size<4||tg_le32(decoded)==0x3072cfa1UL){free(decoded);return 0;}
        ok=tg_account_result(c,decoded,size,kind);free(decoded);return ok;
    }
    if(constructor==0x2144ca19UL)return rpc_error(c,&t,kind);
    if(kind>=TG_REQ_STATE)return tg_model_result(c,p,n,kind);
    if(kind==TG_REQ_EXPORT||kind==TG_REQ_IMPORT){
        if(constructor==TL_auth_loginToken){
            TgU32 expires=tg_u32(&t),now=(TgU32)(time(NULL)+c->time_offset);bytes=tg_bytes(&t,&length);
            if(!bytes||!length||length>48||t.at!=t.n||expires<=now||expires-now>3600)return 0;
            uri(c,bytes,length);c->token_deadline=c->now+(expires-now)*1000UL;
            tg_state(c,TG_QR,"Telegram: Settings > Devices > Link Desktop Device");return 1;
        }
        if(constructor==TL_auth_loginTokenMigrateTo){
            unsigned dc=tg_u32(&t);bytes=tg_bytes(&t,&length);
            if(!bytes||!length||length>48||t.at!=t.n||dc<1||dc>(c->test?3:5))return 0;
            memcpy(c->import_token,bytes,length);c->import_n=length;c->dc=dc;c->has_key=c->authorized=0;tg_erase(c->auth,256);c->qr[0]=0;
            return tg_connect_dc(c);
        }
        if(constructor==TL_auth_loginTokenSuccess)return authorization(c,&t);
    }
    if(kind==TG_REQ_PASSWORD&&constructor==TL_account_password)return password_parameters(c,&t);
    if(kind==TG_REQ_CHECK){t.at=0;return authorization(c,&t);}
    if(kind==TG_REQ_SELF){
        TgU64 id;if(constructor!=0x1cb5c415UL||tg_u32(&t)!=1)return 0;id=user(&t);
        if(!t.ok||!id||(c->user_id&&id!=c->user_id))return 0;c->user_id=id;c->authorized=1;
        if(c->outbox_pending)return tg_rpc(c,TG_REQ_SEND);
        tg_state(c,TG_READY,"Session restored. Saved Messages is ready.");return 1;
    }
    if(kind==TG_REQ_SEND&&(constructor==TL_updates||constructor==TL_updatesCombined||constructor==TL_updateShortSentMessage||constructor==TL_updateShort)) {
        tg_model_sent(c,p,n);
        c->outbox_pending=0;tg_erase(c->outbox,sizeof(c->outbox));c->outbox_n=0;
        tg_state(c,TG_READY,c->model?"Message accepted by Telegram":"Message accepted by Telegram in Saved Messages");return 1;
    }
    return 0;
}
int tg_client_password_key(TgClient *c,int key) {
    TgByte A[256],proof[32];TgTl t;int ok;
    if(c->state!=TG_PASSWORD)return 0;
    if(key==263||key==8||key==127){
        unsigned previous=ru_previous_grapheme(c->password,c->password_n,c->password_n);
        tg_erase(c->password+previous,c->password_n-previous);
        c->password_n=previous;c->revision++;return 1;
    }
    if(key==21){tg_erase(c->password,sizeof(c->password));c->password_n=0;c->revision++;return 1;}
    if(key==10||key==13){
        ok=tg_srp(c->password,c->password_n,c->salt1,c->salt1_n,c->salt2,c->salt2_n,c->g,tg_prime,c->B,A,proof);
        tg_erase(c->password,sizeof(c->password));c->password_n=0;
        if(!ok){tg_fail(c,"Could not construct validated 2FA proof");return 1;}
        tg_tl_init(&t,c->last_query,sizeof(c->last_query));tg_w32(&t,TL_auth_checkPassword);tg_w32(&t,TL_inputCheckPasswordSRP);tg_w64(&t,c->srp_id);tg_wbytes(&t,A,256);tg_wbytes(&t,proof,32);
        tg_erase(A,sizeof(A));tg_erase(proof,sizeof(proof));c->last_n=t.at;c->request=TG_REQ_CHECK;c->retries=0;
        if(!t.ok||!tg_send_encrypted(c,c->last_query,c->last_n,1))tg_fail(c,"Could not send 2FA proof");else tg_state(c,TG_CONNECTING,"Checking 2FA password");return 1;
    }
    if(key>=32&&key<=255&&c->password_n<sizeof(c->password)){c->password[c->password_n++]=key;c->revision++;return 1;}return 0;
}
/* Unicode input stays in erasable C storage, without an RFL password string. */
int tg_client_password_scalar(TgClient *c,TgU32 cp) {
    unsigned char bytes[4];unsigned n;
    if(c->state!=TG_PASSWORD||cp>0x10ffffUL||(cp>=0xd800&&cp<=0xdfff)||ru_width(cp)<0)return 0;
    n=ru_encode(cp,bytes);
    if(c->password_n+n>sizeof(c->password)){tg_erase(bytes,sizeof(bytes));return 0;}
    memcpy(c->password+c->password_n,bytes,n);c->password_n+=n;
    tg_erase(bytes,sizeof(bytes));c->revision++;return 1;
}
int tg_send_to(TgClient *c,const TgByte *text,unsigned n) {
    if(c->state!=TG_READY||!n||n>sizeof(c->outbox)||c->outbox_pending)return 0;
    memcpy(c->outbox,text,n);c->outbox_n=n;c->outbox_pending=1;
    if(!tg_random(c->random_id,8)){tg_fail(c,"Could not prepare message request");return 0;}
    if(c->request){tg_state(c,TG_SENDING,"Message queued while the current request finishes");return 1;}
    if(!tg_rpc(c,TG_REQ_SEND)){tg_fail(c,"Could not prepare message request");return 0;}
    tg_state(c,TG_SENDING,c->model?"Sending message":"Sending to Saved Messages");return 1;
}
int tg_client_send(TgClient *c,const TgByte *text,unsigned n){if(c->state!=TG_READY||c->outbox_pending)return 0;if(c->model)tg_model_saved(c);return tg_send_to(c,text,n);}
