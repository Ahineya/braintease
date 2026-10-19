/* SPDX-License-Identifier: MIT */
#include "model.h"
int tg_accept_pts(TgClient *c,const TgView *v,unsigned slot,int force){TgModel *m=c->model;TgPeer *p=NULL;TgU32 pts,count,current;
    if(force||!m->have_state)return 1;if(!v->fields[F_pts].p)return 1;
    pts=tg_v32(v,F_pts);count=tg_v32(v,F_pts_count);
    if(slot&&m->peers[slot-1].peer.kind==TG_CHANNEL)p=m->peers+slot-1;
    current=p?p->pts:m->pts;if(!current&&p){p->need_diff=1;return 0;}
    if(pts<current||(pts==current&&count))return 0;
    if(pts!=current+count){if(p)p->need_diff=1;else m->need_diff=1;return 0;}
    if(p)p->pts=pts;else m->pts=pts;return 1;
}
static void remove_messages(TgClient *c,const TgView *v,unsigned slot){TgModel *m=c->model;TgTl t;unsigned n,i,j;TgU32 id;
    if(!tg_vector(v->fields[F_messages],&t,&n))return;
    for(i=0;i<n;i++){id=tg_u32(&t);
        if(m->active&&(slot?m->active==slot:m->peers[m->active-1].peer.kind!=TG_CHANNEL)){
            for(j=0;j<m->messages_n;j++)if(m->messages[j].id==id){free(m->messages[j].text);memmove(m->messages+j,m->messages+j+1,(m->messages_n-j-1)*sizeof(TgMessage));m->messages_n--;break;}}
        for(j=0;j<m->peers_n;j++)if(m->peers[j].top==id&&(slot?j+1==slot:m->peers[j].peer.kind!=TG_CHANNEL))strcpy(m->peers[j].preview,"[message deleted]");
    }tg_model_changed(c);
}
static void update(TgClient *c,const TgView *v,int force){TgModel *m=c->model;TgView msg,peer;TgPeerId id={0,0,0};TgPeer *p;unsigned slot=0,n,i;TgTl t;
    if(v->id==TL_updateNewMessage||v->id==TL_updateNewChannelMessage||v->id==TL_updateEditMessage||v->id==TL_updateEditChannelMessage){
        if(!tg_slice_view(v->fields[F_message],&msg))return;id=tg_message_peer(c,&msg);slot=tg_peer_slot(c,id,1);
        if(!slot||!tg_accept_pts(c,v,slot,force))return;p=m->peers+slot-1;
        if(!force&&!(msg.flags[0]&2)&&tg_v32(&msg,F_id)>p->top&&v->id!=TL_updateEditMessage&&v->id!=TL_updateEditChannelMessage)p->unread++;
        tg_message_apply(c,&msg,0,0);
    }else if(v->id==TL_updateDeleteMessages||v->id==TL_updateDeleteChannelMessages){
        if(v->id==TL_updateDeleteChannelMessages){id.kind=TG_CHANNEL;id.id=tg_v64(v,F_channel_id);slot=tg_peer_slot(c,id,1);}
        if(tg_accept_pts(c,v,slot,force))remove_messages(c,v,slot);
    }else if(v->id==TL_updateChannelTooLong){id.kind=TG_CHANNEL;id.id=tg_v64(v,F_channel_id);slot=tg_peer_slot(c,id,1);if(slot)m->peers[slot-1].need_diff=1;
    }else if(v->id==TL_updateReadHistoryInbox||v->id==TL_updateReadChannelInbox){
        if(v->id==TL_updateReadChannelInbox){id.kind=TG_CHANNEL;id.id=tg_v64(v,F_channel_id);}else id=tg_peer(v->fields[F_peer]);slot=tg_peer_slot(c,id,1);
        if(slot&&(v->id==TL_updateReadChannelInbox||tg_accept_pts(c,v,slot,force))){TgU32 unread=tg_v32(v,F_still_unread_count);m->peers[slot-1].unread=unread>65535UL?65535U:(unsigned)unread;tg_model_changed(c);}
    }else if(v->id==TL_updateDialogPinned){
        if(!tg_slice_view(v->fields[F_peer],&peer))return;id=tg_peer(peer.fields[F_peer]);slot=tg_peer_slot(c,id,1);
        if(slot){p=m->peers+slot-1;if(v->flags[0]&1)for(i=0;i<m->peers_n;i++)if(m->peers[i].folder==tg_v32(v,F_folder_id)&&m->peers[i].pin)m->peers[i].pin++;
            p->pin=(v->flags[0]&1)?1:0;p->listed=1;p->folder=tg_v32(v,F_folder_id);tg_model_changed(c);}
    }else if(v->id==TL_updatePinnedDialogs){
        if(!v->fields[F_order].p){m->stage=1;return;}
        n=tg_v32(v,F_folder_id);for(i=0;i<m->peers_n;i++)if(m->peers[i].folder==n)m->peers[i].pin=0;
        if(tg_vector(v->fields[F_order],&t,&n))for(i=0;i<n&&tg_view(&t,&peer);i++){id=tg_peer(peer.fields[F_peer]);slot=tg_peer_slot(c,id,1);if(slot){m->peers[slot-1].pin=i+1;m->peers[slot-1].listed=1;m->peers[slot-1].folder=tg_v32(v,F_folder_id);}}
        tg_model_changed(c);
    }else if(v->id==TL_updateFolderPeers){
        if(!tg_accept_pts(c,v,0,force))return;
        if(tg_vector(v->fields[F_folder_peers],&t,&n))for(i=0;i<n&&tg_view(&t,&peer);i++){id=tg_peer(peer.fields[F_peer]);slot=tg_peer_slot(c,id,1);if(slot)m->peers[slot-1].folder=tg_v32(&peer,F_folder_id);}tg_model_changed(c);
    }else if(v->id==TL_updateShortMessage||v->id==TL_updateShortChatMessage){
        char *text;id.kind=v->id==TL_updateShortMessage?TG_USER:TG_CHAT;id.id=tg_v64(v,v->id==TL_updateShortMessage?F_user_id:F_chat_id);slot=tg_peer_slot(c,id,1);
        if(!slot||!tg_accept_pts(c,v,slot,force))return;text=malloc(4097);if(!text)return;tg_display(text,4097,tg_vtext(v,F_message),1);
        if(v->flags[0]&2){id.kind=TG_USER;id.id=c->user_id;}else if(v->id==TL_updateShortChatMessage){id.kind=TG_USER;id.id=tg_v64(v,F_from_id);}
        p=m->peers+slot-1;if(!(v->flags[0]&2)&&tg_v32(v,F_id)>p->top)p->unread++;
        tg_add_message(c,slot,tg_v32(v,F_id),tg_v32(v,F_date),id,(v->flags[0]&2)!=0,text,0);free(text);
        if(!p->peer.hash&&p->peer.kind!=TG_CHAT&&p->peer.id!=c->user_id)m->stage=1;
    }else{
        /* Non-text updates still advance their message-box sequence. */
        if(v->fields[F_channel_id].p){id.kind=TG_CHANNEL;id.id=tg_v64(v,F_channel_id);slot=tg_peer_slot(c,id,1);}
        tg_accept_pts(c,v,slot,force);
        if(v->fields[F_qts].p&&!force)m->need_diff=1;
    }
}
void tg_apply_updates(TgClient *c,const TgView *v,int force){TgTl t;TgView item;TgSlice s;unsigned n,i;
    if(v->fields[F_update].p){if(tg_slice_view(v->fields[F_update],&item))update(c,&item,force);return;}
    s=v->fields[v->fields[F_other_updates].p?F_other_updates:F_updates];
    if(tg_vector(s,&t,&n))for(i=0;i<n&&tg_view(&t,&item);i++)update(c,&item,force);
}
int tg_model_update(TgClient *c,TgByte *data,unsigned size){TgModel *m=c->model;TgTl t;TgView v;TgU32 seq,start;if(!m)return 1;
    tg_tl_init(&t,data,size);if(!tg_view(&t,&v)||t.at!=t.n){char error[128];snprintf(error,sizeof(error),"Invalid Telegram update object %08lx",(unsigned long)tg_tl_error());tg_fail(c,error);return 0;}
    if(v.id==TL_updatesTooLong){m->need_diff=1;return 1;}
    seq=tg_v32(&v,F_seq);start=v.fields[F_seq_start].p?tg_v32(&v,F_seq_start):seq;
    if(m->have_state&&seq){if(seq<=m->seq)return 1;if(start>m->seq+1){m->need_diff=1;return 1;}}
    tg_entities(c,&v);
    if(v.id==TL_updateShortMessage||v.id==TL_updateShortChatMessage)update(c,&v,0);else tg_apply_updates(c,&v,0);
    if(m->have_state&&seq&&!m->need_diff)m->seq=seq;if(v.fields[F_date].p)m->date=tg_v32(&v,F_date);return 1;
}
void tg_model_sent(TgClient *c,TgByte *data,unsigned size){TgModel *m=c->model;TgView v;TgTl t;TgPeerId sender;unsigned slot;char text[2049];if(!m)return;
    tg_tl_init(&t,data,size);if(!tg_view(&t,&v)||t.at!=t.n){m->need_diff=1;return;}
    if(v.id==TL_updateShortSentMessage){
        slot=tg_peer_slot(c,m->outbox_peer,1);if(!slot){sender.kind=TG_USER;sender.id=c->user_id;sender.hash=0;slot=tg_peer_slot(c,sender,1);}
        sender.kind=TG_USER;sender.id=c->user_id;sender.hash=0;memcpy(text,c->outbox,c->outbox_n);text[c->outbox_n]=0;
        tg_add_message(c,slot,tg_v32(&v,F_id),tg_v32(&v,F_date),sender,1,text,0);tg_accept_pts(c,&v,slot,0);
    }else tg_model_update(c,data,size);
    m->outbox_peer.kind=0;
}
