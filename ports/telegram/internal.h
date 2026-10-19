/* SPDX-License-Identifier: MIT */
#ifndef TG_INTERNAL_H
#define TG_INTERNAL_H
#include "client.h"
#include "packet.h"
#include "schema.h"
#include "../../rfl/os/socket.h"
#include <stdio.h>
#include <sys/stat.h>
#define TG_REQ_EXPORT 1
#define TG_REQ_IMPORT 2
#define TG_REQ_PASSWORD 3
#define TG_REQ_CHECK 4
#define TG_REQ_SELF 5
#define TG_REQ_SEND 6
#define TG_REQ_STATE 7
#define TG_REQ_PINS 8
#define TG_REQ_DIALOGS 9
#define TG_REQ_HISTORY 10
#define TG_REQ_DIFF 11
#define TG_REQ_CHANNEL_DIFF 12
typedef struct TgModel TgModel;
typedef struct {
    TgByte nonce[16],server_nonce[16],new_nonce[32],tmp_key[32],tmp_iv[32];
    TgByte pq[8],p[4],q[4],public_a[256],exponent[256],key[256],aux[20],retry[8];
    unsigned npq,np,nq,g,retries;
} TgHandshake;
struct TgClient {
    unsigned state,dc,test,has_key,authorized,initialized,phase,request,retries;
    unsigned tx_n,tx_at,rx_n,last_n,ack_n,seen_n,seen_at,import_n,password_n,salt1_n,salt2_n,g;
    unsigned outbox_n,outbox_pending,login_signal,key_restarts,crypto_checked;
    unsigned long revision,deadline,token_deadline,now;
    TgU32 api_id,sequence,last_seq;int32_t time_offset;
    TgU64 last_id,request_id,user_id,srp_id,seen[64],acks[64];
    char api_hash[33],session_path[256],status[160],qr[85],hint[256];
    TgByte auth[256],salt[8],session[8],import_token[48],password[512],salt1[128],salt2[128],B[256];
    TgByte outbox[2048],random_id[8],last_query[4096];
    TgByte tx[TG_MAX+4],rx[TG_MAX+4],scratch[TG_MAX],unpacked[TG_MAX];
    RflPoll *poll;void *driver;RflWait *connecting;RflSocket *socket;TgHandshake *handshake;
    TgModel *model;
#ifdef TG_TESTING
    TgByte fixture_ip[4];unsigned fixture_port;
#endif
};
void tg_fail(TgClient *,const char *);
void tg_state(TgClient *,unsigned,const char *);
TgU64 tg_message_id(TgClient *);
int tg_queue(TgClient *,const TgByte *,unsigned);
int tg_send_plain(TgClient *,const TgByte *,unsigned);
int tg_send_encrypted(TgClient *,const TgByte *,unsigned,int);
int tg_handshake_start(TgClient *);
int tg_handshake_receive(TgClient *,TgByte *,unsigned);
void tg_handshake_free(TgClient *);
int tg_rpc(TgClient *,unsigned);
int tg_receive(TgClient *,TgByte *,unsigned);
int tg_store(TgClient *);
int tg_load(TgClient *);
int tg_connect_dc(TgClient *);
int tg_config(TgClient *,const char *);
int tg_account_result(TgClient *,TgByte *,unsigned,unsigned);
void tg_transport_close(TgClient *);
void tg_model_free(TgClient *);
void tg_model_step(TgClient *);
int tg_model_query(TgClient *,TgTl *,unsigned);
int tg_model_result(TgClient *,TgByte *,unsigned,unsigned);
int tg_model_update(TgClient *,TgByte *,unsigned);
void tg_model_reconnect(TgClient *);
void tg_model_error(TgClient *,unsigned,const char *);
void tg_model_sent(TgClient *,TgByte *,unsigned);
void tg_model_send_peer(TgClient *,TgTl *);
void tg_model_saved(TgClient *);
int tg_send_to(TgClient *,const TgByte *,unsigned);
#endif
