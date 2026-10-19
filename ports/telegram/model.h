/* SPDX-License-Identifier: MIT */
#ifndef TG_MODEL_H
#define TG_MODEL_H
#include "internal.h"
#include "tl.h"
#define TG_PEERS 2048
#define TG_HISTORY 192
#define TG_USER 1
#define TG_CHAT 2
#define TG_CHANNEL 3
typedef struct {unsigned kind;TgU64 id,hash;} TgPeerId;
typedef struct {
    TgPeerId peer;unsigned listed,pin,unread,folder,writable,need_diff;
    TgU32 top,date,pts;char title[128],username[64],preview[160];
} TgPeer;
typedef struct {TgU32 id,date;TgPeerId sender;unsigned out,edited;char *text;} TgMessage;
struct TgModel {
    TgPeer peers[TG_PEERS];unsigned peers_n,order[TG_PEERS],order_n,dirty;
    TgMessage messages[TG_HISTORY];unsigned messages_n,active,history_peer,history_pending,history_older,history_end;
    TgPeerId outbox_peer,offset_peer;TgU32 offset_date,offset_id,history_offset;
    TgU32 history_generation,history_request_generation;
    unsigned folder,stage,have_state,need_diff,channel_peer,page_count;
    TgU32 pts,qts,date,seq;unsigned long next_sync,retry_at,page_after;
    char filter[256],line[4100];
};
TgPeerId tg_peer(TgSlice);
TgPeerId tg_message_peer(TgClient *,const TgView *);
void tg_write_peer(TgClient *,TgTl *,TgPeerId);
unsigned tg_peer_slot(TgClient *,TgPeerId,int);
void tg_entities(TgClient *,const TgView *);
void tg_dialogs_apply(TgClient *,const TgView *,int);
void tg_messages_apply(TgClient *,TgSlice,unsigned,int);
void tg_message_apply(TgClient *,const TgView *,unsigned,int);
void tg_add_message(TgClient *,unsigned,TgU32,TgU32,TgPeerId,unsigned,const char *,int);
void tg_model_changed(TgClient *);
void tg_clear_history(TgClient *);
void tg_display(char *,unsigned,TgSlice,int);
void tg_apply_updates(TgClient *,const TgView *,int);
void tg_update_state(TgModel *,const TgView *);
int tg_accept_pts(TgClient *,const TgView *,unsigned,int);
#endif
