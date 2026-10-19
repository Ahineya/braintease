/* Exercise the actual Unix profile with the independent packet codec fixture. */
#define RIPPLE_FUZIX_NETWORK
#include "../../../c-test/tests/runtime/net_tcp_test.h"
#define C(x) do {if(!(x)){printf("tcp_window failed line %u\n",__LINE__);return 1;}}while(0)
static unsigned char result[NET_TCP_RX_BUFFER];
int main(void) {
    unsigned short n,i;unsigned long first;
    C(NET_TCP_RX_BUFFER==4096&&sizeof(NetTcp)*4<=64000UL);
    reset();start();
    C(read16(outgoing+48)==NET_TCP_RX_BUFFER&&read16(outgoing+56)==1460);
    packet(18,peer_next-1,tcp.snd_nxt,65535U,0);tick(1);
    for(i=0;i<1460;i++)data[i]=i&255;
    packet(24,tcp.rcv_nxt,tcp.snd_nxt,65535U,1460);tick(2);
    C(net_tcp_read(&tcp,result,sizeof(result),&n)==NET_OK&&n==1460);
    for(i=0;i<n;i++)C(result[i]==(i&255));
    reset();start();
    packet(18,peer_next-1,tcp.snd_nxt,65535U,0);tick(1);first=tcp.rcv_nxt;
    for(i=0;i<8;i++){packet(24,tcp.rcv_nxt,tcp.snd_nxt,65535U,512);tick(now+1);}
    C(tcp.rx_used==4096&&tcp.rcv_nxt==first+4096&&read16(outgoing+48)==0);
    packet(17,tcp.rcv_nxt,tcp.snd_nxt,65535U,0);tick(now+1);C(!tcp.peer_fin);
    C(net_tcp_read(&tcp,result,3000,&n)==NET_OK&&n==3000);
    for(i=0;i<n;i++)C(result[i]==(i&255));
    net_tcp_poll(&tcp,now+1);C(read16(outgoing+48)==3000);
    /* A partial drain followed by another burst wraps the RX ring. */
    for(i=0;i<5;i++){packet(24,tcp.rcv_nxt,tcp.snd_nxt,65535U,512);tick(now+1);}
    C(tcp.rx_used==3656&&tcp.bytes_received==6656);
    C(net_tcp_read(&tcp,result,sizeof(result),&n)==NET_OK&&n==3656);
    for(i=0;i<n;i++)C(result[i]==((i+3000)&255));
    packet(17,tcp.rcv_nxt,tcp.snd_nxt,65535U,0);tick(now+1);
    C(tcp.peer_fin&&net_tcp_read(&tcp,result,sizeof(result),&n)==NET_EOF);
    C(net.received_frames==16);
    puts("large TCP window passed");return 0;
}
