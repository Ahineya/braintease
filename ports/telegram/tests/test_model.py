#!/usr/bin/env python3
"""Model/schema tests with realistic layer-230 optional fields and update gaps."""
import ctypes as C
from pathlib import Path
import re
import unittest
from support import build, ROOT, tl, u32, u64, Reader

IDS={name:int(value,16) for name,value in re.findall(r'^#define TL_(\w+) 0x([0-9a-f]+)UL', (ROOT/'ports/telegram/schema.h').read_text(),re.M)}
def obj(name,*parts):return u32(IDS[name])+b''.join(parts)
def vec(*parts):return u32(0x1cb5c415)+u32(len(parts))+b''.join(parts)
def peer(kind,ident):return obj('peer'+kind,u64(ident))
def user(ident,name,username=''):
    return obj('user',u32(1|2|8|32|64),u32(0),u64(ident),u64(ident*111),tl(name.encode()),tl(username.encode()),obj('userProfilePhotoEmpty'),obj('userStatusOffline',u32(123)))
def channel(ident,title):
    return obj('channel',u32((1<<13)|(1<<5)),u32(0),u64(ident),u64(ident*111),tl(title.encode()),obj('chatPhotoEmpty'),u32(123))
def message(ident,text,kind='User',chat=7,sender=7,out=False,media=False,date=1000):
    flags=(2 if out else 0)|256|(512 if media else 0)|128
    return obj('message',u32(flags),u32(0),u32(ident),peer('User',sender),peer(kind,chat),u32(date),tl(text.encode()),
        obj('messageMediaPhoto',u32(1),obj('photoEmpty',u64(987))) if media else b'',
        vec(obj('messageEntityBold',u32(0),u32(min(len(text),2)))))
def dialog(kind,ident,top,pin=False,pts=0,folder=0):
    flags=(4 if pin else 0)|(1 if pts else 0)|(16 if folder else 0)
    return obj('dialog',u32(flags),peer(kind,ident),u32(top),u32(0),u32(0),u32(3),u32(0),u32(0),u32(0),obj('peerNotifySettings',u32(0)),u32(pts) if pts else b'',u32(folder) if folder else b'')
def state(pts=10,seq=1):return obj('updates_state',u32(pts),u32(0),u32(1000),u32(seq),u32(0))
def updates(*items,seq=0):return obj('updates',vec(*items),vec(),vec(),u32(1001),u32(seq))

class ModelTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.lib,_,_=build();lib=cls.lib
        lib.tg_test_model.restype=C.c_void_p
        lib.tg_test_model_debug.argtypes=[C.c_void_p];lib.tg_test_model_debug.restype=C.c_char_p
        for name in ['tg_model_result','tg_model_update','tg_model_sent']:
            getattr(lib,name).argtypes=[C.c_void_p,C.c_void_p,C.c_uint]+([C.c_uint] if name=='tg_model_result' else [])
        for name in ['tg_client_chat_count','tg_client_can_send','tg_client_history_more']:
            getattr(lib,name).argtypes=[C.c_void_p]
        for name in ['tg_client_chat_key','tg_client_chat_flags','tg_client_chat_unread','tg_client_open','tg_client_history_lines']:
            getattr(lib,name).argtypes=[C.c_void_p,C.c_uint]
        for name in ['tg_client_chat_title','tg_client_chat_preview']:
            getattr(lib,name).argtypes=[C.c_void_p,C.c_uint];getattr(lib,name).restype=C.c_char_p
        lib.tg_client_active_title.argtypes=[C.c_void_p];lib.tg_client_active_title.restype=C.c_char_p
        lib.tg_client_history_line.argtypes=[C.c_void_p,C.c_uint,C.c_uint];lib.tg_client_history_line.restype=C.c_char_p
        lib.tg_client_filter.argtypes=[C.c_void_p,C.c_char_p]
        lib.tg_client_latest.argtypes=[C.c_void_p]
        lib.tg_client_send_active.argtypes=[C.c_void_p,C.c_void_p,C.c_uint]
        lib.tg_test_last_query.argtypes=[C.c_void_p];lib.tg_test_last_query.restype=C.c_void_p
        lib.tg_test_model_query.argtypes=[C.c_void_p,C.c_uint,C.c_void_p,C.c_uint]
        lib.tg_test_channel.argtypes=[C.c_void_p,C.c_uint64]
        lib.tg_test_recipient.argtypes=[C.c_void_p];lib.tg_test_recipient.restype=C.c_uint64
    def setUp(self):self.c=self.lib.tg_test_model();self.assertTrue(self.c);self.feed(state(),7)
    def tearDown(self):self.lib.tg_client_free(self.c)
    def feed(self,data,kind=None):
        p=C.create_string_buffer(data)
        result=self.lib.tg_model_update(self.c,p,len(data)) if kind is None else self.lib.tg_model_result(self.c,p,len(data),kind)
        self.assertEqual(result,1)
    def titles(self):return [self.lib.tg_client_chat_title(self.c,i).decode() for i in range(self.lib.tg_client_chat_count(self.c))]
    def row(self,name):return self.titles().index(name)
    def history(self):return '\n'.join(self.lib.tg_client_history_line(self.c,i,60).decode() for i in range(self.lib.tg_client_history_lines(self.c,60)))
    def query(self,kind):
        p=C.create_string_buffer(4096);n=self.lib.tg_test_model_query(self.c,kind,p,4096);self.assertGreater(n,0);return Reader(p.raw[:n])
    def load(self):
        self.feed(obj('messages_peerDialogs',vec(dialog('User',7,9,True)),vec(message(9,'pinned preview')),vec(),vec(user(7,'Alice','alice_user')),state()),8)
        self.feed(obj('messages_dialogs',vec(dialog('Channel',50,3,pts=20),dialog('User',8,12)),vec(message(3,'Channel photo','Channel',50,media=True),message(12,'Bob preview',chat=8,date=2000)),vec(channel(50,'News')),vec(user(8,'Bob'),user(9,'Not a dialog'))),9)
    def test_pins_search_entities_and_offsets(self):
        self.load();self.assertEqual(self.titles(),['Alice','Bob','News','Saved Messages'])
        self.assertEqual(self.lib.tg_client_chat_flags(self.c,0)&1,1)
        self.lib.tg_client_filter(self.c,b'ALICE_USER');self.assertEqual(self.titles(),['Alice'])
        self.lib.tg_client_filter(self.c,b'zzz');self.assertEqual(self.titles(),[])
        self.lib.tg_client_filter(self.c,b'');self.assertEqual(len(self.titles()),4)
    def test_history_media_control_safety_edit_delete_and_private_recipient(self):
        self.load();self.lib.tg_client_open(self.c,self.row('Alice'));self.query(10)
        self.feed(obj('messages_messages',vec(message(9,'caption\nsecond line',media=True),message(8,'old\x1b[31m')),vec(),vec(),vec(user(7,'Alice'))),10)
        text=self.history();self.assertLess(text.index('old?'),text.index('caption'));self.assertIn('second line',text);self.assertNotIn('\x1b',text)
        # An incoming private message may name our own user as recipient.
        self.feed(updates(obj('updateNewMessage',message(10,'incoming DM',chat=42,sender=7),u32(11),u32(1)),seq=2))
        self.assertIn('incoming DM',self.history());self.assertIn('pts=11',self.lib.tg_test_model_debug(self.c).decode())
        self.feed(updates(obj('updateEditMessage',message(10,'edited DM',chat=42,sender=7),u32(12),u32(1)),seq=3))
        self.assertIn('edited DM',self.history());self.assertNotIn('incoming DM',self.history())
        self.feed(updates(obj('updateDeleteMessages',vec(u32(10)),u32(13),u32(1)),seq=4));self.assertNotIn('edited DM',self.history())
    def test_global_gap_recovery_and_duplicate_suppression(self):
        self.load();self.lib.tg_client_open(self.c,self.row('Bob'));self.query(10)
        packet=updates(obj('updateNewMessage',message(15,'gap payload',chat=8,sender=8),u32(13),u32(1)),seq=4)
        self.feed(packet);self.assertNotIn('gap payload',self.history());self.assertIn('diff=1',self.lib.tg_test_model_debug(self.c).decode())
        self.feed(obj('updates_difference',vec(message(15,'gap payload',chat=8,sender=8)),vec(),vec(),vec(),vec(),state(13,4)),11)
        self.assertEqual(self.history().count('gap payload'),1);self.feed(packet);self.assertEqual(self.history().count('gap payload'),1)
        self.assertIn('diff=0',self.lib.tg_test_model_debug(self.c).decode())
    def test_channel_id_isolation_readonly_and_difference(self):
        self.load();self.assertTrue(self.lib.tg_client_open(self.c,self.row('News')));self.assertFalse(self.lib.tg_client_can_send(self.c));self.query(10)
        self.feed(obj('messages_channelMessages',u32(0),u32(20),u32(1),vec(message(3,'channel history','Channel',50)),vec(),vec(),vec()),10)
        self.feed(updates(obj('updateNewChannelMessage',message(4,'lost channel event','Channel',50),u32(22),u32(1))))
        self.assertNotIn('lost channel event',self.history())
        self.lib.tg_test_channel(self.c,50)
        self.feed(obj('updates_channelDifference',u32(1),u32(22),vec(message(4,'recovered channel event','Channel',50)),vec(),vec(),vec()),12)
        self.assertIn('recovered channel event',self.history())
        self.feed(updates(obj('updateDeleteMessages',vec(u32(3)),u32(11),u32(1)),seq=2));self.assertIn('channel history',self.history())
    def test_selected_recipient_is_frozen_while_switching_chats(self):
        self.load();self.lib.tg_client_open(self.c,self.row('Bob'))
        self.assertTrue(self.lib.tg_client_send_active(self.c,b'test body',9))
        self.assertFalse(self.lib.tg_client_send(self.c,b'other body',10))
        self.assertEqual(self.lib.tg_test_recipient(self.c),8)
        data=C.string_at(self.lib.tg_test_last_query(self.c),100);r=Reader(data)
        self.assertEqual(r.integer(),IDS['invokeWithLayer']);r.integer();self.assertEqual(r.integer(),IDS['initConnection'])
        r.integer();r.integer()
        for _ in range(6):r.blob()
        self.assertEqual(r.integer(),IDS['messages_sendMessage']);self.assertEqual(r.integer(),2);self.assertEqual(r.integer(),IDS['inputPeerUser']);self.assertEqual(r.integer(8),8);self.assertEqual(r.integer(8),888)
        self.lib.tg_client_open(self.c,self.row('Alice'))
        sent=obj('updateShortSentMessage',u32(2),u32(16),u32(11),u32(1),u32(1001))
        self.lib.tg_model_sent(self.c,C.create_string_buffer(sent),len(sent));self.assertNotIn('test body',self.history())
    def test_stale_history_and_dialog_responses_do_not_regress_channel_pts_or_offsets(self):
        self.load();self.lib.tg_client_open(self.c,self.row('News'));self.query(10)
        self.feed(updates(obj('updateNewChannelMessage',message(4,'first live channel message','Channel',50,date=3000),u32(21),u32(1))))
        self.feed(obj('messages_channelMessages',u32(0),u32(20),u32(1),vec(message(3,'stale history','Channel',50,date=1000)),vec(),vec(),vec()),10)
        self.feed(updates(obj('updateNewChannelMessage',message(5,'second live channel message','Channel',50,date=4000),u32(22),u32(1))))
        self.assertIn('second live channel message',self.history())
        self.feed(obj('messages_dialogsSlice',u32(4),vec(dialog('Channel',50,3,pts=20)),vec(message(3,'stale history','Channel',50,date=1000)),vec(),vec()),9)
        r=self.query(9);self.assertEqual(r.integer(),IDS['messages_getDialogs']);r.integer();r.integer()
        self.assertEqual(r.integer(),1000);self.assertEqual(r.integer(),3)
    def test_broadcast_administrator_can_post_text(self):
        self.load()
        admin=obj('channel',u32((1<<13)|(1<<5)|(1<<14)),u32(0),u64(50),u64(5550),tl(b'News'),obj('chatPhotoEmpty'),u32(123),obj('chatAdminRights',u32(2)))
        self.feed(obj('messages_dialogs',vec(dialog('Channel',50,3,pts=20)),vec(),vec(admin),vec()),9)
        self.lib.tg_client_open(self.c,self.row('News'));self.assertTrue(self.lib.tg_client_can_send(self.c))
    def test_latest_reload_ignores_an_older_inflight_history_reply(self):
        self.load();self.lib.tg_client_open(self.c,self.row('Bob'));self.query(10)
        self.lib.tg_client_latest(self.c)
        self.feed(obj('messages_messages',vec(message(8,'stale older page',chat=8,sender=8)),vec(),vec(),vec()),10)
        self.assertNotIn('stale older page',self.history())
        self.query(10)
        self.feed(obj('messages_messages',vec(message(12,'fresh latest page',chat=8,sender=8)),vec(),vec(),vec()),10)
        self.assertIn('fresh latest page',self.history())
    def test_unicode_survives_titles_messages_grapheme_wrapping_and_safe_clipping(self):
        self.feed(obj('messages_peerDialogs',vec(dialog('User',7,9)),vec(),vec(),vec(user(7,'Жанна🙂')),state()),8)
        self.assertIn('Жанна🙂',self.titles());self.lib.tg_client_open(self.c,self.row('Жанна🙂'));self.query(10)
        value='Привет é中🙂!' * 3
        self.feed(obj('messages_messages',vec(message(9,value)),vec(),vec(),vec()),10)
        lines=[self.lib.tg_client_history_line(self.c,i,10).decode() for i in range(self.lib.tg_client_history_lines(self.c,10))]
        self.assertEqual(''.join(lines[1:-1]),value)
        for line in lines[1:-1]:
            self.assertFalse(line.startswith('́'));self.assertNotIn('�',line)
        self.lib.tg_client_filter(self.c,'Жан'.encode());self.assertEqual(self.titles(),['Жанна🙂'])
        class Slice(C.Structure):_fields_=[('p',C.c_void_p),('n',C.c_uint)]
        self.lib.tg_display.argtypes=[C.c_void_p,C.c_uint,Slice,C.c_int]
        raw='aЖé🙂'.encode();data=C.create_string_buffer(raw)
        for capacity in range(1,14):
            out=C.create_string_buffer(capacity);self.lib.tg_display(out,capacity,Slice(C.cast(data,C.c_void_p),len(raw)),0)
            value=out.value.decode();self.assertFalse(value.endswith('e'))

    def test_malformed_schema_never_partially_applies(self):
        before=self.titles();bad=obj('updates',u32(0x1cb5c415),u32(0xffffffff))
        self.assertEqual(self.lib.tg_model_update(self.c,C.create_string_buffer(bad),len(bad)),0);self.assertEqual(self.titles(),before)

if __name__=='__main__':unittest.main()
