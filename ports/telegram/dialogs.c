/* SPDX-License-Identifier: MIT */
#include "model.h"
static int expired(unsigned long now,unsigned long end){return (TgU32)(now-end)<0x80000000UL;}
void tg_update_state(TgModel *m,const TgView *v){m->pts=tg_v32(v,F_pts);m->qts=tg_v32(v,F_qts);m->date=tg_v32(v,F_date);m->seq=tg_v32(v,F_seq);m->have_state=1;}
void tg_model_reconnect(TgClient *c){if(c->model){c->model->need_diff=c->model->have_state;c->model->next_sync=0;}}
void tg_model_step(TgClient *c){TgModel *m=c->model;unsigned i,kind=0;if(!m||!c->authorized||c->state!=TG_READY||c->request)return;
    if(m->retry_at&&!expired(c->now,m->retry_at))return;m->retry_at=0;
    if(!m->have_state)kind=TG_REQ_STATE;
    else if(m->need_diff)kind=TG_REQ_DIFF;
    else if(m->history_pending&&m->stage>=2)kind=TG_REQ_HISTORY;
    else if(m->stage==1||m->stage==3){if(m->page_after&&!expired(c->now,m->page_after))return;kind=TG_REQ_PINS;}
    else if(m->stage==2||m->stage==4){if(m->page_after&&!expired(c->now,m->page_after))return;kind=TG_REQ_DIALOGS;}
    else{
        for(i=0;i<m->peers_n;i++)if(m->peers[i].need_diff&&m->peers[i].peer.kind==TG_CHANNEL&&m->peers[i].peer.hash){m->channel_peer=i+1;kind=TG_REQ_CHANNEL_DIFF;break;}
        if(!kind&&expired(c->now,m->next_sync)){m->need_diff=1;kind=TG_REQ_DIFF;}
    }
    if(kind&&!tg_rpc(c,kind))tg_fail(c,"Could not encode chat request");
}
int tg_model_query(TgClient *c,TgTl *t,unsigned kind){TgModel *m=c->model;TgPeer *p;if(!m)return 0;
    if(kind==TG_REQ_STATE)tg_w32(t,TL_updates_getState);
    else if(kind==TG_REQ_PINS){m->folder=m->stage==3;tg_w32(t,TL_messages_getPinnedDialogs);tg_w32(t,m->folder);}
    else if(kind==TG_REQ_DIALOGS){
        tg_w32(t,TL_messages_getDialogs);tg_w32(t,3);tg_w32(t,m->folder);tg_w32(t,m->offset_date);tg_w32(t,m->offset_id);
        if(m->offset_peer.kind)tg_write_peer(c,t,m->offset_peer);else tg_w32(t,TL_inputPeerEmpty);tg_w32(t,8);tg_w64(t,0);
    }else if(kind==TG_REQ_HISTORY){
        if(!m->active)return 0;p=m->peers+m->active-1;m->history_peer=m->active;
        m->history_request_generation=m->history_generation;
        m->history_offset=m->history_older&&m->messages_n?m->messages[0].id:0;m->history_pending=0;
        tg_w32(t,TL_messages_getHistory);tg_write_peer(c,t,p->peer);tg_w32(t,m->history_offset);tg_w32(t,0);tg_w32(t,0);tg_w32(t,8);tg_w32(t,0);tg_w32(t,0);tg_w64(t,0);
    }else if(kind==TG_REQ_DIFF){
        tg_w32(t,TL_updates_getDifference);tg_w32(t,3);tg_w32(t,m->pts);tg_w32(t,10);tg_w32(t,20);tg_w32(t,m->date);tg_w32(t,m->qts);
    }else if(kind==TG_REQ_CHANNEL_DIFF){
        p=m->peers+m->channel_peer-1;tg_w32(t,TL_updates_getChannelDifference);tg_w32(t,0);tg_w32(t,TL_inputChannel);tg_w64(t,p->peer.id);tg_w64(t,p->peer.hash);
        tg_w32(t,TL_channelMessagesFilterEmpty);tg_w32(t,p->pts);tg_w32(t,10);
    }else return 0;return t->ok;
}
void tg_dialogs_apply(TgClient *c,const TgView *v,int pins){TgModel *m=c->model;TgTl t;TgView d;TgPeerId id;TgPeer *p;unsigned n,i,slot;
    tg_entities(c,v);if(!tg_vector(v->fields[F_dialogs],&t,&n))return;
    if(pins)for(i=0;i<m->peers_n;i++)if(m->peers[i].folder==m->folder)m->peers[i].pin=0;
    for(i=0;i<n&&tg_view(&t,&d);i++){
        if(d.id!=TL_dialog)continue;id=tg_peer(d.fields[F_peer]);slot=tg_peer_slot(c,id,1);if(!slot)continue;p=m->peers+slot-1;
        p->listed=1;if(tg_v32(&d,F_top_message)>=p->top){TgU32 unread=tg_v32(&d,F_unread_count);p->top=tg_v32(&d,F_top_message);p->unread=unread>65535UL?65535U:(unsigned)unread;}p->folder=d.fields[F_folder_id].p?tg_v32(&d,F_folder_id):m->folder;
        if(pins)p->pin=i+1;else if(d.flags[0]&4){if(!p->pin)p->pin=512+i;}
        if(d.fields[F_pts].p&&tg_v32(&d,F_pts)>p->pts)p->pts=tg_v32(&d,F_pts);
        if(!pins){m->offset_peer=p->peer;m->offset_id=tg_v32(&d,F_top_message);}
    }
    tg_messages_apply(c,v->fields[F_messages],0,0);
    if(!pins&&m->offset_peer.kind){
        slot=tg_peer_slot(c,m->offset_peer,0);if(slot)m->offset_date=m->peers[slot-1].date;
        if(tg_vector(v->fields[F_messages],&t,&n))for(i=0;i<n&&tg_view(&t,&d);i++){
            id=tg_message_peer(c,&d);if(id.kind==m->offset_peer.kind&&id.id==m->offset_peer.id&&tg_v32(&d,F_id)==m->offset_id)m->offset_date=tg_v32(&d,F_date);
        }
    }
    tg_model_changed(c);
}
static void next_folder(TgModel *m){m->stage=m->folder?5:3;m->offset_id=m->offset_date=0;m->offset_peer.kind=0;m->page_count=0;}
int tg_model_result(TgClient *c,TgByte *data,unsigned size,unsigned kind){TgModel *m=c->model;TgView v,state;TgTl t;unsigned n,previous_stage;TgU32 old,old_date;TgPeerId old_peer;
    if(!m)return 0;previous_stage=m->stage;tg_tl_init(&t,data,size);if(!tg_view(&t,&v)||t.at!=t.n){char error[128];snprintf(error,sizeof(error),"Invalid Telegram response object %08lx (request %u)",(unsigned long)tg_tl_error(),kind);tg_fail(c,error);return 0;}
    if(kind==TG_REQ_STATE&&v.id==TL_updates_state){tg_update_state(m,&v);if(!m->stage)m->stage=1;m->next_sync=c->now+25000UL;}
    else if(kind==TG_REQ_PINS&&v.id==TL_messages_peerDialogs){tg_dialogs_apply(c,&v,1);m->stage++;m->offset_id=m->offset_date=0;m->offset_peer.kind=0;m->page_count=0;}
    else if(kind==TG_REQ_DIALOGS&&(v.id==TL_messages_dialogs||v.id==TL_messages_dialogsSlice)){
        old=m->offset_id;old_date=m->offset_date;old_peer=m->offset_peer;tg_dialogs_apply(c,&v,0);n=0;tg_vector(v.fields[F_dialogs],&t,&n);m->page_count++;
        m->page_after=c->now+1000UL;
        if(v.id==TL_messages_dialogs||!n||!m->offset_peer.kind||!m->offset_date||(old==m->offset_id&&old_date==m->offset_date&&old_peer.kind==m->offset_peer.kind&&old_peer.id==m->offset_peer.id&&m->page_count>1)||m->page_count>=256||m->peers_n>=TG_PEERS-16)next_folder(m);
    }else if(kind==TG_REQ_HISTORY&&(v.id==TL_messages_messages||v.id==TL_messages_messagesSlice||v.id==TL_messages_channelMessages)){
        tg_entities(c,&v);if(m->history_peer==m->active&&m->history_request_generation==m->history_generation){
            old=m->messages_n?m->messages[0].id:0;tg_messages_apply(c,v.fields[F_messages],m->active,m->history_offset!=0);
            n=0;tg_vector(v.fields[F_messages],&t,&n);if(!n||(m->history_offset&&m->messages_n&&m->messages[0].id>=old))m->history_end=1;
            if(v.id==TL_messages_channelMessages&&tg_v32(&v,F_pts)>m->peers[m->active-1].pts)m->peers[m->active-1].pts=tg_v32(&v,F_pts);
        }
    }else if(kind==TG_REQ_DIFF){
        if(v.id==TL_updates_differenceEmpty){m->date=tg_v32(&v,F_date);m->seq=tg_v32(&v,F_seq);m->need_diff=0;}
        else if(v.id==TL_updates_difference||v.id==TL_updates_differenceSlice){
            tg_entities(c,&v);tg_messages_apply(c,v.fields[F_new_messages],0,0);tg_apply_updates(c,&v,1);
            if(!tg_slice_view(v.fields[v.id==TL_updates_difference?F_state:F_intermediate_state],&state))return 0;tg_update_state(m,&state);m->need_diff=v.id==TL_updates_differenceSlice;
        }else if(v.id==TL_updates_differenceTooLong){tg_clear_history(c);m->have_state=0;m->need_diff=0;m->stage=1;m->history_pending=1;m->history_older=0;}
        else return 0;m->next_sync=c->now+25000UL;
    }else if(kind==TG_REQ_CHANNEL_DIFF){
        TgPeer *p=m->peers+m->channel_peer-1;
        if(v.id==TL_updates_channelDifference||v.id==TL_updates_channelDifferenceTooLong){
            tg_entities(c,&v);tg_messages_apply(c,v.fields[v.id==TL_updates_channelDifference?F_new_messages:F_messages],0,0);tg_apply_updates(c,&v,1);
            if(v.id==TL_updates_channelDifferenceTooLong&&tg_slice_view(v.fields[F_dialog],&state))p->pts=tg_v32(&state,F_pts);
        }else if(v.id!=TL_updates_channelDifferenceEmpty)return 0;
        if(v.fields[F_pts].p)p->pts=tg_v32(&v,F_pts);p->need_diff=!(v.flags[0]&1);
    }else return 0;
    if((previous_stage!=5&&m->stage==5)||!strncmp(c->status,"Telegram error",14))tg_state(c,TG_READY,m->peers_n>=TG_PEERS-16?"Chat cache limit reached; some chats may not be listed.":m->stage==5?"Chats ready. Incoming text is live.":"Connected. Loading chats...");
    tg_model_changed(c);return 1;
}
void tg_model_error(TgClient *c,unsigned kind,const char *error){TgModel *m=c->model;unsigned long seconds;if(!m)return;
    if(!strncmp(error,"FLOOD_WAIT_",11)){seconds=strtoul(error+11,NULL,10);if(seconds>3600)seconds=3600;m->retry_at=c->now+(seconds+1)*1000UL;if(kind==TG_REQ_HISTORY)m->history_pending=1;return;}
    if(kind==TG_REQ_PINS){m->stage++;m->offset_id=m->offset_date=0;m->offset_peer.kind=0;}
    else if(kind==TG_REQ_DIALOGS)next_folder(m);
    else if(kind==TG_REQ_HISTORY)m->history_end=1;
    else if(kind==TG_REQ_CHANNEL_DIFF){m->peers[m->channel_peer-1].need_diff=0;m->history_pending=1;}
    else if(kind==TG_REQ_DIFF){m->need_diff=0;m->next_sync=c->now+60000UL;}
    else if(kind==TG_REQ_STATE)m->retry_at=c->now+30000UL;
    tg_model_changed(c);
}
