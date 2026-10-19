/* SPDX-License-Identifier: MIT */
#include "model.h"
#include "../../runtime/include/ripple_unicode.h"
static int same(TgPeerId a,TgPeerId b){return a.kind==b.kind&&a.id==b.id;}
void tg_model_changed(TgClient *c){c->model->dirty=1;c->revision++;}
void tg_display(char *out,unsigned cap,TgSlice s,int multiline) {
    size_t at=0,i=0,start=0; unsigned j,n; uint32_t cp;
    unsigned char bytes[4]; struct ru_break state={0,0,0,0,0,0}; int joins;
    if(!cap)return;
    while(i<s.n) {
        cp=ru_next(s.p,s.n,&i);
        if(cp=='\r')continue;
        if(cp=='\n')cp=multiline?'\n':' ';
        else if(cp=='\t')cp=' ';
        else if(cp<32||(cp>=127&&cp<160))cp='?';
        joins=ru_join(&state,cp);if(!joins)start=at;
        n=ru_encode(cp,bytes);
        if(at+n>=cap){if(joins)at=start;break;}
        for(j=0;j<n;j++)out[at++]=bytes[j];
    }
    out[at]=0;
}
TgPeerId tg_peer(TgSlice s) {
    TgPeerId p={0,0,0};TgU32 id;if(!s.p||s.n!=12)return p;id=tg_le32(s.p);
    p.kind=id==TL_peerUser?TG_USER:id==TL_peerChat?TG_CHAT:id==TL_peerChannel?TG_CHANNEL:0;p.id=tg_le64(s.p+4);return p;
}
TgPeerId tg_message_peer(TgClient *c,const TgView *v){TgPeerId id=tg_peer(v->fields[F_peer_id]),from=tg_peer(v->fields[F_from_id]);
    if(!(v->flags[0]&2)&&id.kind==TG_USER&&id.id==c->user_id&&from.kind==TG_USER&&from.id!=c->user_id)id=from;return id;}
void tg_write_peer(TgClient *c,TgTl *t,TgPeerId p) {
    if(p.kind==TG_USER&&p.id==c->user_id)tg_w32(t,TL_inputPeerSelf);
    else if(p.kind==TG_USER&&p.hash){tg_w32(t,TL_inputPeerUser);tg_w64(t,p.id);tg_w64(t,p.hash);}
    else if(p.kind==TG_CHAT){tg_w32(t,TL_inputPeerChat);tg_w64(t,p.id);}
    else if(p.kind==TG_CHANNEL&&p.hash){tg_w32(t,TL_inputPeerChannel);tg_w64(t,p.id);tg_w64(t,p.hash);}
    else t->ok=0;
}
unsigned tg_peer_slot(TgClient *c,TgPeerId p,int create) {
    TgModel *m=c->model;TgPeer *v;unsigned i;
    if(!p.kind||!p.id)return 0;
    for(i=0;i<m->peers_n;i++)if(same(m->peers[i].peer,p)){if(p.hash)m->peers[i].peer.hash=p.hash;return i+1;}
    if(!create||m->peers_n==TG_PEERS)return 0;
    i=m->peers_n++;v=m->peers+i;memset(v,0,sizeof(*v));v->peer=p;
    snprintf(v->title,sizeof(v->title),"%s %lu",p.kind==TG_USER?"User":"Chat",(unsigned long)(TgU32)p.id);
    v->writable=p.kind==TG_CHAT||p.hash||(p.kind==TG_USER&&p.id==c->user_id);
    if(p.kind==TG_USER&&p.id==c->user_id)strcpy(v->title,"Saved Messages");
    return i+1;
}
static void entities(TgClient *c,TgSlice s) {
    TgTl t;TgView v,rights;TgPeerId id;TgPeer *p;TgSlice a;unsigned n,i,slot,admin;char last[128];
    if(!s.p||!tg_vector(s,&t,&n))return;
    for(i=0;i<n&&tg_view(&t,&v);i++){
        id.kind=(v.id==TL_user||v.id==TL_userEmpty)?TG_USER:(v.id==TL_chat||v.id==TL_chatForbidden||v.id==TL_chatEmpty)?TG_CHAT:(v.id==TL_channel||v.id==TL_channelForbidden)?TG_CHANNEL:0;
        id.id=tg_v64(&v,F_id);id.hash=tg_v64(&v,F_access_hash);
        if((id.kind==TG_USER&&(v.flags[0]&(1UL<<20)))||(id.kind==TG_CHANNEL&&(v.flags[0]&(1UL<<12))))id.hash=0;
        slot=tg_peer_slot(c,id,1);if(!slot)continue;p=c->model->peers+slot-1;
        if(id.kind==TG_USER){
            a=tg_vtext(&v,F_first_name);if(a.p){tg_display(p->title,sizeof(p->title),a,0);
                a=tg_vtext(&v,F_last_name);if(a.n){tg_display(last,sizeof(last),a,0);if(strlen(p->title)+strlen(last)+2<sizeof(p->title)){strcat(p->title," ");strcat(p->title,last);}}}
            if(v.flags[0]&(1UL<<13)){strcpy(p->title,"Deleted account");p->writable=0;}
            else if(id.hash)p->writable=1;
            if(id.id==c->user_id){strcpy(p->title,"Saved Messages");p->writable=1;}
        }else{
            a=tg_vtext(&v,F_title);if(a.p)tg_display(p->title,sizeof(p->title),a,0);
            if(v.id==TL_channel){
                admin=tg_slice_view(v.fields[F_admin_rights],&rights);
                p->writable=!(v.flags[0]&(1UL<<5))||(v.flags[0]&1)||(admin&&(rights.flags[0]&2));
                if(tg_slice_view(v.fields[F_banned_rights],&rights)&&(rights.flags[0]&((1UL<<1)|(1UL<<25))))p->writable=0;
                if(!(v.flags[0]&1)&&!admin&&tg_slice_view(v.fields[F_default_banned_rights],&rights)&&(rights.flags[0]&((1UL<<1)|(1UL<<25))))p->writable=0;
                if(v.flags[0]&(1UL<<2))p->writable=0;
            }else if(v.id==TL_chat)p->writable=!(v.flags[0]&((1UL<<2)|(1UL<<5)));
            else p->writable=0;
        }
        a=tg_vtext(&v,F_username);if(a.p)tg_display(p->username,sizeof(p->username),a,0);
    }
}
void tg_entities(TgClient *c,const TgView *v){entities(c,v->fields[F_users]);entities(c,v->fields[F_chats]);}
void tg_add_message(TgClient *c,unsigned slot,TgU32 id,TgU32 date,TgPeerId sender,unsigned out,const char *text,int older) {
    TgModel *m=c->model;TgPeer *p;TgMessage *msg;unsigned i,at;size_t size;char *copy;
    if(!slot||!id)return;p=m->peers+slot-1;
    if(id>=p->top){p->top=id;p->date=date;tg_display(p->preview,sizeof(p->preview),(TgSlice){(TgByte *)text,strlen(text)},0);for(i=0;p->preview[i];i++)if(p->preview[i]=='\n')p->preview[i]=' ';}
    p->listed=1;
    if(slot!=m->active){tg_model_changed(c);return;}
    size=strlen(text)+1;copy=malloc(size);if(!copy)return;memcpy(copy,text,size);
    for(i=0;i<m->messages_n;i++)if(m->messages[i].id==id)break;
    if(i<m->messages_n){
        msg=m->messages+i;if(msg->text&&msg->date==date&&!strcmp(msg->text,text)){free(copy);return;}
        free(msg->text);msg->text=NULL;msg->edited=1;
    }else{
        if(m->messages_n==TG_HISTORY){
            at=older?m->messages_n-1:0;free(m->messages[at].text);
            if(!older)memmove(m->messages,m->messages+1,(m->messages_n-1)*sizeof(*msg));m->messages_n--;
        }
        for(i=0;i<m->messages_n&&m->messages[i].id<id;i++){}
        memmove(m->messages+i+1,m->messages+i,(m->messages_n-i)*sizeof(*msg));m->messages_n++;msg=m->messages+i;memset(msg,0,sizeof(*msg));
    }
    msg->text=copy;msg->id=id;msg->date=date;msg->sender=sender;msg->out=out;tg_model_changed(c);
}
void tg_message_apply(TgClient *c,const TgView *v,unsigned expected,int older) {
    TgPeerId id=tg_message_peer(c,v),sender=tg_peer(v->fields[F_from_id]);unsigned slot;char *text;TgSlice s;
    if(v->id==TL_messageEmpty)return;
    if(v->id!=TL_message&&v->id!=TL_messageService)return;
    slot=id.kind?tg_peer_slot(c,id,1):expected;if(!slot)return;
    if(expected&&slot!=expected)return;
    text=malloc(4100);if(!text)return;
    s=tg_vtext(v,F_message);
    if(v->id==TL_messageService)strcpy(text,"[service message]");
    else if(s.n)tg_display(text,4097,s,1);
    else strcpy(text,v->fields[F_media].p?"[media message]":"[empty message]");
    if(!sender.kind){sender=c->model->peers[slot-1].peer;if(v->flags[0]&2){sender.kind=TG_USER;sender.id=c->user_id;sender.hash=0;}}
    tg_add_message(c,slot,tg_v32(v,F_id),tg_v32(v,F_date),sender,(v->flags[0]&2)!=0,text,older);free(text);
}
void tg_messages_apply(TgClient *c,TgSlice s,unsigned expected,int older){TgTl t;TgView v;unsigned n,i;if(!s.p||!tg_vector(s,&t,&n))return;
    for(i=0;i<n&&tg_view(&t,&v);i++)tg_message_apply(c,&v,expected,older);}
static int contains(const char *text,const char *query){unsigned i,j;int a,b;if(!*query)return 1;
    for(i=0;text[i];i++){for(j=0;query[j]&&text[i+j];j++){a=text[i+j];b=query[j];if(a>='A'&&a<='Z')a+=32;if(b>='A'&&b<='Z')b+=32;if(a!=b)break;}if(!query[j])return 1;}return 0;}
static int before(const TgPeer *a,const TgPeer *b){if(a->folder!=b->folder)return a->folder<b->folder;
    if(!!a->pin!=!!b->pin)return a->pin!=0;if(a->pin&&b->pin)return a->pin<b->pin;
    if(a->date!=b->date)return a->date>b->date;return a->peer.id<b->peer.id;}
static void order(TgClient *c){TgModel *m=c->model;unsigned i,j,gap,key;if(!m||!m->dirty)return;m->order_n=0;
    for(i=0;i<m->peers_n;i++)if(m->peers[i].listed&&(contains(m->peers[i].title,m->filter)||contains(m->peers[i].username,m->filter))){
        m->order[m->order_n++]=i;}
    for(gap=m->order_n/2;gap;gap/=2)for(i=gap;i<m->order_n;i++){key=m->order[i];j=i;
        while(j>=gap&&before(m->peers+key,m->peers+m->order[j-gap])){m->order[j]=m->order[j-gap];j-=gap;}m->order[j]=key;}
    m->dirty=0;}
int tg_client_browse(TgClient *c){TgModel *m;TgPeerId id;if(!c->authorized)return 0;if(c->model)return 1;
    m=calloc(1,sizeof(*m));if(!m)return 0;c->model=m;m->dirty=1;id.kind=TG_USER;id.id=c->user_id;id.hash=0;m->active=tg_peer_slot(c,id,1);
    m->peers[m->active-1].listed=1;m->history_pending=1;tg_model_changed(c);return 1;}
void tg_model_free(TgClient *c){unsigned i;if(!c->model)return;for(i=0;i<c->model->messages_n;i++)free(c->model->messages[i].text);tg_erase(c->model,sizeof(*c->model));free(c->model);c->model=NULL;}
void tg_clear_history(TgClient *c){TgModel *m=c->model;unsigned i;for(i=0;i<m->messages_n;i++)free(m->messages[i].text);m->messages_n=0;m->history_end=0;m->history_generation++;}
void tg_client_filter(TgClient *c,const char *query){if(!c->model)return;tg_display(c->model->filter,sizeof(c->model->filter),(TgSlice){(TgByte *)query,strlen(query)},0);tg_model_changed(c);}
unsigned tg_client_chat_count(TgClient *c){order(c);return c->model?c->model->order_n:0;}
unsigned tg_client_chat_key(TgClient *c,unsigned row){order(c);return c->model&&row<c->model->order_n?c->model->order[row]+1:0;}
const char *tg_client_chat_title(TgClient *c,unsigned row){unsigned p=tg_client_chat_key(c,row);return p?c->model->peers[p-1].title:"";}
const char *tg_client_chat_preview(TgClient *c,unsigned row){unsigned p=tg_client_chat_key(c,row);return p?c->model->peers[p-1].preview:"";}
unsigned tg_client_chat_flags(TgClient *c,unsigned row){unsigned p=tg_client_chat_key(c,row);return p?(c->model->peers[p-1].pin?1:0)|(c->model->peers[p-1].folder?2:0):0;}
unsigned tg_client_chat_unread(TgClient *c,unsigned row){unsigned p=tg_client_chat_key(c,row);return p?c->model->peers[p-1].unread:0;}
int tg_client_open(TgClient *c,unsigned row){unsigned p=tg_client_chat_key(c,row);TgModel *m=c->model;if(!p)return 0;
    if(m->peers[p-1].peer.kind!=TG_CHAT&&!m->peers[p-1].peer.hash&&!(m->peers[p-1].peer.kind==TG_USER&&m->peers[p-1].peer.id==c->user_id)){tg_state(c,TG_READY,"Chat access details unavailable; refreshing dialogs");m->stage=1;return 0;}
    if(m->active==p)return 1;tg_clear_history(c);m->active=p;m->history_older=0;m->history_pending=1;tg_model_changed(c);return 1;}
unsigned tg_client_active_key(TgClient *c){return c->model?c->model->active:0;}
const char *tg_client_active_title(TgClient *c){TgModel *m=c->model;return m&&m->active?m->peers[m->active-1].title:"Choose a chat";}
int tg_client_can_send(TgClient *c){TgModel *m=c->model;return (c->state==TG_READY||c->state==TG_SENDING)&&m&&m->active&&m->peers[m->active-1].writable;}
int tg_client_send_active(TgClient *c,const TgByte *text,unsigned n){TgModel *m=c->model;if(!tg_client_can_send(c)||c->state!=TG_READY||c->outbox_pending)return 0;
    m->outbox_peer=m->peers[m->active-1].peer;return tg_send_to(c,text,n);}
void tg_model_saved(TgClient *c){TgModel *m=c->model;m->outbox_peer.kind=TG_USER;m->outbox_peer.id=c->user_id;m->outbox_peer.hash=0;}
void tg_model_send_peer(TgClient *c,TgTl *t){if(c->model&&c->model->outbox_peer.kind)tg_write_peer(c,t,c->model->outbox_peer);else tg_w32(t,TL_inputPeerSelf);}
int tg_client_history_more(TgClient *c){TgModel *m=c->model;if(!m||!m->active||m->history_end)return 0;m->history_pending=1;m->history_older=1;c->revision++;return 1;}
void tg_client_latest(TgClient *c){TgModel *m=c->model;if(!m||!m->active)return;tg_clear_history(c);m->history_pending=1;m->history_older=0;tg_model_changed(c);}
/* The same iterator drives line counts and extraction; widths are terminal
 * columns and every break is a complete extended-grapheme boundary. */
static size_t history_end(const char *text,size_t length,size_t at,unsigned width) {
    size_t next;unsigned columns=0,w;
    while(at<length&&text[at]!='\n') {
        next=ru_next_grapheme((const unsigned char *)text,length,at,&w);
        if(columns+w>width)break;
        columns+=w;at=next;
    }
    return at;
}
static unsigned wrapped(const char *s,unsigned width){
    size_t at=0,end,length=strlen(s);unsigned lines=1;
    while(at<length){end=history_end(s,length,at,width);if(end==length)break;
        at=end+(s[end]=='\n');lines++;}
    return lines;
}
unsigned tg_client_history_lines(TgClient *c,unsigned width){TgModel *m=c->model;unsigned i,n=0;if(!m||width<10||width>180)return 0;
    for(i=0;i<m->messages_n;i++)if(m->messages[i].text)n+=2+wrapped(m->messages[i].text,width);return n;}
const char *tg_client_history_line(TgClient *c,unsigned line,unsigned width){
    TgModel *m=c->model;TgMessage *msg;unsigned i,n,slot;size_t at,end,length,count;TgU32 time;const char *name,*s;
    if(!m||width<10||width>180)return "";m->line[0]=0;
    for(i=0;i<m->messages_n;i++){
        msg=m->messages+i;if(!msg->text)continue;n=2+wrapped(msg->text,width);if(line>=n){line-=n;continue;}
        if(!line){slot=tg_peer_slot(c,msg->sender,0);name=msg->out?"You":slot?m->peers[slot-1].title:"Someone";time=msg->date%86400UL;
            snprintf(m->line,sizeof(m->line),"%s  %02lu:%02lu%s",name,(unsigned long)(time/3600),(unsigned long)(time/60%60),msg->edited?" (edited)":"");return m->line;}
        if(line==n-1)return "";line--;s=msg->text;length=strlen(s);at=0;
        for(;;){end=history_end(s,length,at,width);if(!line)break;line--;at=end+(end<length&&s[end]=='\n');}
        count=end-at;
        if(count>=sizeof(m->line))count=ru_prefix((const unsigned char *)s+at,end-at,width);
        /* Messages are bounded to 4096 octets; line[] can hold any one line. */
        memcpy(m->line,s+at,count);m->line[count]=0;return m->line;
    }return "";
}
