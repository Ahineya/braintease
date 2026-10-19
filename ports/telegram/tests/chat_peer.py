"""Independent encrypted API fixture for dialogs, paging and pushed messages."""
import threading
import time
import os
from test_login import Peer
from test_model import obj,vec,peer,user,channel,message,dialog,state,updates,IDS
from support import u32,u64

class ChatPeer(Peer):
    def __init__(self,*args):
        super().__init__(*args)
        self.write_lock=threading.RLock();self.exit_ok=threading.Event();self.account_ready=threading.Event()
        self.live_pts=10;self.live_seq=1;self.recipients=[];self.history_requests=[]
        self.incoming_salt=os.urandom(8)
    def send(self,*args,**kwargs):
        kwargs['header_salt']=self.incoming_salt
        with self.write_lock:return super().send(*args,**kwargs)
    def push(self):
        with self.write_lock:
            self.live_pts+=1;self.live_seq+=1
            self.send(updates(obj('updateNewMessage',message(20,'Live incoming fixture text',chat=42,sender=8,date=int(time.time())),u32(self.live_pts),u32(1)),seq=self.live_seq))
    def input_peer(self,r):
        kind=r.integer()
        if kind==IDS['inputPeerSelf']:return 'User',42
        if kind==IDS['inputPeerChat']:return 'Chat',r.integer(8)
        assert kind in (IDS['inputPeerUser'],IDS['inputPeerChannel'])
        ident=r.integer(8);assert r.integer(8)==ident*111
        return ('User' if kind==IDS['inputPeerUser'] else 'Channel'),ident
    def account_loop(self,restored):
        self.connection.settimeout(60);self.exit_ok.clear();self.account_ready.set()
        while True:
            try:ident,_,method,r=self.request()
            except (EOFError,ConnectionResetError):
                assert self.exit_ok.is_set() or self.received,'unexpected fixture client close'
                return
            if method==IDS['updates_getState']:
                r.done();answer=state(self.live_pts,self.live_seq)
            elif method==IDS['messages_getPinnedDialogs']:
                folder=r.integer();r.done()
                answer=obj('messages_peerDialogs',vec(dialog('User',7,9,True)) if not folder else vec(),vec(message(9,'Pinned chat preview')),vec(),vec(user(7,'Alice','alice_user')),state(self.live_pts,self.live_seq))
            elif method==IDS['messages_getDialogs']:
                assert r.integer()==3;folder=r.integer();r.integer();offset=r.integer();kind=r.integer()
                if kind!=IDS['inputPeerEmpty']:
                    r.integer(8)
                    if kind in (IDS['inputPeerUser'],IDS['inputPeerChannel']):r.integer(8)
                assert r.integer()==8 and r.integer(8)==0;r.done()
                if not folder and not offset:
                    answer=obj('messages_dialogsSlice',u32(3),vec(dialog('User',8,12),dialog('Channel',50,3,pts=20)),vec(message(12,'Bob preview',chat=8,sender=8),message(3,'Channel caption','Channel',50,media=True)),vec(channel(50,'News')),vec(user(8,'Bob')))
                elif folder:
                    answer=obj('messages_dialogs',vec(dialog('User',9,6,folder=1)),vec(message(6,'Archived message',chat=9,sender=9)),vec(),vec(user(9,'Archived friend')))
                else:answer=obj('messages_dialogs',vec(),vec(),vec(),vec())
            elif method==IDS['messages_getHistory']:
                kind,chat=self.input_peer(r);offset=r.integer();self.history_requests.append((chat,offset))
                assert r.integer()==0 and r.integer()==0 and r.integer()==8 and r.integer()==0 and r.integer()==0 and r.integer(8)==0;r.done()
                if offset>1:items=[message(1,'Earliest fixture history',kind,chat,sender=chat)]
                elif offset:items=[]
                elif chat==42:items=[message(5,'Existing Saved Messages history',chat=42,sender=42,out=True)]
                elif chat==8:items=([message(20,'Live incoming fixture text',chat=42,sender=8)] if self.live_pts>10 else [])+[message(12,'Bob history',chat=8,sender=8),message(8,'Older Bob history',chat=8,sender=8)]
                else:items=[message(3,'Channel caption' if kind=='Channel' else 'Alice history',kind,chat,sender=chat,media=kind=='Channel')]
                answer=obj('messages_messages',vec(*items),vec(),vec(),vec())
            elif method==IDS['updates_getDifference']:
                assert r.integer()==3;r.integer();r.integer();r.integer();r.integer();r.integer();r.done()
                answer=obj('updates_differenceEmpty',u32(int(time.time())),u32(self.live_seq))
            elif method==IDS['messages_sendMessage']:
                assert r.integer()==2;target=self.input_peer(r);body=r.blob();assert r.take(8)!=bytes(8);r.done()
                if body==b'reject fixture draft':
                    self.result(ident,u32(0x2144ca19)+u32(403)+__import__('support').tl(b'CHAT_WRITE_FORBIDDEN'));continue
                self.recipients.append(target);self.received.append(body);self.live_pts+=1
                answer=obj('updateShortSentMessage',u32(2),u32(21),u32(self.live_pts),u32(1),u32(int(time.time())))
            else:raise AssertionError('Unexpected chat method '+hex(method))
            self.result(ident,answer,compressed=True)
