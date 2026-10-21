#include "net_tcp_test.h"
static unsigned syn_valid;
static unsigned char sendbuf[4096], recvbuf[4096], mapbuf[512], result[4096];
static void open_window(void) {
    reset();
    net_tcp_connect(&net,&tcp,&target,now);
    net_tcp_buffers(&tcp,sendbuf,sizeof(sendbuf),recvbuf,sizeof(recvbuf),mapbuf);
    net_tcp_poll(&tcp,now);
    syn_valid=outgoing[47]==2&&read16(outgoing+48)==4096;
    packet(18,peer_next-1,tcp.snd_nxt,4096,0);tick(10);
}
int main(void) {
    unsigned short n,i,frames;unsigned long first,acked,repeated;
    open_window();
    check(syn_valid&&tcp.active&&NET_TCP_RX_CAP(&tcp)==4096&&read16(outgoing+48)==4096);
    check(net_tcp_buffers(&tcp,sendbuf,4096,recvbuf,4096,mapbuf)==NET_BUSY);
    for(i=0;i<2000;i++)sendbuf[i]=i&255;
    frames=transmissions;
    check(net_tcp_write(&tcp,sendbuf,2000,&n)==NET_OK&&n==2000);
    net_tcp_poll(&tcp,11);
    check(transmissions>=frames+2&&tcp.flight_data>tcp.mss&&tcp.flight_data<=tcp.cwnd&&tcp.sent_nxt==tcp.snd_nxt);
    first=tcp.snd_una;acked=tcp.sent_nxt;
    packet(16,tcp.rcv_nxt,acked,4096,0);tick(20);
    check(tcp.snd_una==acked&&tcp.bytes_acked==acked-first&&tcp.tx_used==2000-(acked-first));
    for(i=0;i<tcp.tx_used;i++)if(NET_TCP_TX_DATA(&tcp)[i]!=((i+acked-first)&255))break;
    check(i==tcp.tx_used&&wire_ok());
    /* Three duplicate ACKs trigger the oldest missing segment before RTO. */
    first=tcp.snd_una;repeated=tcp.retransmits;
    for(i=0;i<3;i++){packet(16,tcp.rcv_nxt,first,4096,0);tick(21+i);}
    check(tcp.recovering&&tcp.retransmits==repeated+1&&read32(outgoing+38)==first);
    packet(16,tcp.rcv_nxt,first+64,4096,0);tick(25);
    check(tcp.snd_una==first+64&&tcp.recovering&&read32(outgoing+38)==first+64);
    packet(16,tcp.rcv_nxt,tcp.sent_nxt,4096,0);tick(30);
    check(!tcp.recovering&&tcp.cwnd<=NET_TCP_TX_CAP(&tcp));
    open_window();first=tcp.rcv_nxt;
    /* Retain out-of-order data once, including overlapping duplicates. */
    packet(24,first+128,tcp.snd_nxt,4096,128);tick(11);
    check(!tcp.rx_used&&tcp.reorder_used==128&&tcp.rcv_nxt==first);
    for(i=0;i<128;i++)data[i]=99;
    packet(24,first+128,tcp.snd_nxt,4096,128);tick(12);
    check(tcp.reorder_used==128);
    for(i=0;i<128;i++)data[i]=i&255;
    packet(24,first,tcp.snd_nxt,4096,128);tick(13);
    check(tcp.rx_used==256&&!tcp.reorder_used&&tcp.rcv_nxt==first+256);
    check(net_tcp_read(&tcp,result,4096,&n)==NET_OK&&n==256);
    for(i=0;i<n;i++)if(result[i]!=(i&127))break;
    check(i==n);
    /* A FIN behind a gap is not lost, nor exposed before its data. */
    first=tcp.rcv_nxt;
    packet(25,first+64,tcp.snd_nxt,4096,64);tick(14);
    check(tcp.pending_fin&&!tcp.peer_fin&&!tcp.rx_used);
    packet(24,first,tcp.snd_nxt,4096,64);tick(15);
    check(tcp.peer_fin&&tcp.rx_used==128&&tcp.rcv_nxt==first+129);
    check(net_tcp_read(&tcp,result,4096,&n)==NET_OK&&n==128);
    check(net_tcp_read(&tcp,result,4096,&n)==NET_EOF&&n==0);
    open_window();first=tcp.rcv_nxt;
    packet(17,first+64,tcp.snd_nxt,4096,0);tick(11);
    packet(24,first+64,tcp.snd_nxt,4096,128);tick(12);
    packet(24,first,tcp.snd_nxt,4096,128);tick(13);
    check(tcp.peer_fin&&tcp.rx_used==64&&tcp.rcv_nxt==first+65&&!tcp.reorder_used);
    /* No ACK may cover a segment still blocked in the local NIC queue. */
    open_window();blocked=1;
    net_tcp_write(&tcp,sendbuf,1024,&n);net_tcp_poll(&tcp,11);
    first=tcp.snd_una;
    packet(16,tcp.rcv_nxt,tcp.snd_nxt,4096,0);tick(12);
    check(tcp.queued&&tcp.snd_una==first&&!tcp.bytes_acked);
    blocked=0;tick(13);
    check(!tcp.queued&&tcp.sent_nxt==tcp.snd_nxt&&tcp.flight_data>tcp.mss);
    repeated=tcp.retransmits;tick(2013);
    check(tcp.retransmits>repeated&&tcp.cwnd==tcp.mss);
    putchar('\n');return 0;
}
