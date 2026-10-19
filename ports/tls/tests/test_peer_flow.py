import sys
import unittest
import struct
from pathlib import Path
sys.path.insert(0,str(Path(__file__).resolve().parents[2]/'fuzix/tools'))
from network_peer import Peer, Stream, wire


class Owner:
    address=None
    tcp_services={}
    def __init__(self):self.frames=[]
    def ip(self,protocol,packet):return packet
    def send(self,packet):self.frames.append(packet)


class FlowTests(unittest.TestCase):
    def test_new_syn_replaces_a_closed_tuple_but_retransmit_does_not(self):
        peer=Peer.__new__(Peer)
        peer.address=None;peer.streams={};peer.frames=0;peer.tcp_services={443:lambda s:None}
        def syn(sequence):
            tcp=bytearray(struct.pack('!HHIIBBHHH',50000,443,sequence,0,80,2,512,0,0))
            tcp[16:18]=struct.pack('!H',wire.checksum(wire.GUEST_IP+wire.HOST_IP+struct.pack('!BBH',0,6,len(tcp))+tcp))
            ip=bytearray(struct.pack('!BBHHHBBH4s4s',0x45,0,40,1,0,64,6,0,wire.GUEST_IP,wire.HOST_IP))
            ip[10:12]=struct.pack('!H',wire.checksum(ip))
            peer.packet(wire.HOST_MAC+wire.GUEST_MAC+b'\x08\x00'+ip+tcp)
            return peer.streams[(443,50000)]
        old=syn(100)
        self.assertIs(syn(100),old)
        old.closed=True
        new=syn(200)
        self.assertIsNot(new,old)
        self.assertEqual(new.peer_next,201)
        self.assertIs(syn(200),new)

    def test_queued_data_with_zero_window_prevents_fin(self):
        owner=Owner();stream=Stream(owner,443,1234,passive=True,isn=100)
        owner.address='fixture';stream.established=True
        stream.queue(b'data');stream.window=0;stream.closing=True
        stream.progress()
        self.assertFalse(stream.fin_sent)
        self.assertEqual(owner.frames,[])
        stream.window=4;stream.progress()
        self.assertEqual(owner.frames[-1][20:],b'data')
        self.assertFalse(stream.fin_sent)
        stream.packet(101,stream.next,16,4,b'')
        self.assertTrue(stream.fin_sent)
        self.assertTrue(owner.frames[-1][13]&1)


if __name__=='__main__':unittest.main()
