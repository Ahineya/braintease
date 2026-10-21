/* Checked DEVICE_NET_* mailbox bridge. No C16 pointer reaches DMA storage.
 * Invalid descriptors/buffers have no NIC queue or payload side effects. */
#include "net.h"
static int network_span(U p, U n, size_t *at) {
    U bank=p>>16,offset=p&65535;
    if(bank>=256 || offset>=BANK)return 0;
    *at=bank*BANK+offset;
    return *at>=32 && n<=CELLS-*at;
}
static void network_device(Context *c,U command) {
    U p=((U)c->desc_bank<<16)|c->desc_offset;
    size_t at,buffer=0;
    if(!network_span(p,2,&at)){c->device_status=4;return;}
    uint16_t *d=c->cells+at;
    if(d[0]!=1){c->device_status=2;return;}
    unsigned expected=command==0x100?18:command==0x101?8:command==0x102?9:27;
    if(d[1]!=expected){c->device_status=3;return;}
    if(!network_span(p,expected,&at)){c->device_status=4;return;}
    if(d[2]){c->device_status=5;return;}
    uint8_t frame[NIC_FRAME];
    if(command==0x101 || command==0x102) {
        if(d[command==0x101?7:8]){c->device_status=5;return;}
        unsigned n=d[5];
        if(command==0x101 && (n<14 || n>NIC_FRAME)){c->device_status=3;return;}
        if(!network_span(((U)d[3]<<16)|d[4],n,&buffer) ||
            (buffer<at+expected && at<buffer+n)){c->device_status=4;return;}
        if(command==0x101) {
            for(unsigned i=0;i<n;++i) {
                if(c->cells[buffer+i]>255){c->device_status=3;return;}
                frame[i]=c->cells[buffer+i];
            }
            d[6]=nic_tx(frame,n);
        } else {
            unsigned length=0;
            d[7]=nic_rx(frame,n<NIC_FRAME?n:NIC_FRAME,&length);d[6]=length;
            if(d[7]==NIC_OK) {
                dirty(c,buffer,length);
                for(unsigned i=0;i<length;++i)c->cells[buffer+i]=frame[i];
            }
        }
    } else {
        NicInfo info;nic_info(&info);
        if(command==0x100) {
            d[3]=info.link;d[4]=1500;d[5]=NIC_FRAME;
            for(unsigned i=0;i<6;++i)d[6+i]=info.mac[i];
            d[12]=d[13]=NIC_FRAMES;d[14]=d[16]=0;d[15]=d[17]=1;
        } else {
            d[3]=info.rx_frames;d[4]=info.tx_frames;
            d[5]=info.rx_octets;d[6]=info.rx_octets>>16;
            d[7]=info.tx_octets;d[8]=info.tx_octets>>16;
            for(unsigned i=0;i<9;++i){d[9+2*i]=info.counters[i];d[10+2*i]=info.counters[i]>>16;}
        }
    }
    dirty(c,at,expected);c->device_status=0;
}
