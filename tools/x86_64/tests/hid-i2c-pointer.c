#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../boot/hid_i2c.h"
/* Standard HID mouse and precision touchpad structures, not a fabricated
 * claim about the HP report descriptor. Physical capture is a separate fixture. */
static const uint8_t mouse[]={
0x05,1,0x09,2,0xa1,1,0x85,1,0x09,1,0xa1,0,
0x05,9,0x19,1,0x29,3,0x15,0,0x25,1,0x95,3,0x75,1,0x81,2,
0x95,1,0x75,5,0x81,1,
0x05,1,0x09,0x30,0x09,0x31,0x09,0x38,0x15,0x81,0x25,0x7f,
0x75,8,0x95,3,0x81,6,0xc0,0xc0};
static const uint8_t touch[]={
0x05,0x0d,0x09,5,0xa1,1,0x85,4,
0x09,0x22,0xa1,2,0x09,0x42,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,
0x75,7,0x81,1,0x05,1,0x09,0x30,0x09,0x31,0x16,0,0,0x26,0xff,0x0f,
0x75,16,0x95,2,0x81,2,0xc0,
0x05,0x0d,0x09,0x22,0xa1,2,0x09,0x42,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,
0x75,7,0x81,1,0x05,1,0x09,0x30,0x09,0x31,0x16,0,0,0x26,0xff,0x0f,
0x75,16,0x95,2,0x81,2,0xc0,
0x05,9,0x09,1,0x15,0,0x25,1,0x75,1,0x95,1,0x81,2,0x75,7,0x81,1,0xc0};
static void parsing(void){
    HidI2cLayout l;HidI2cPointer e;
    assert(!hid_i2c_parse_descriptor(&l,mouse,sizeof(mouse)));
    assert(l.numbered && l.report_bits[0][1]==32);
    const uint8_t a[]={1,3,0xfb,7,0xff};
    assert(hid_i2c_decode(&l,a,sizeof(a),&e)==1);
    assert(e.x==-5 && e.y==7 && e.wheel==-1 && e.buttons==3 && !e.absolute && e.contact);
    assert(hid_i2c_decode(&l,a,sizeof(a)-1,&e)==-1);
    assert(!hid_i2c_parse_descriptor(&l,touch,sizeof(touch)));
    const uint8_t b[]={4,0,0xff,0xff,0xff,0xff,1,0x34,0x02,0x89,0x01,1};
    assert(hid_i2c_decode(&l,b,sizeof(b),&e)==1);
    assert(e.x==0x234 && e.y==0x189 && e.absolute && e.contact && e.buttons==1);
    uint8_t c[sizeof(b)];memcpy(c,b,sizeof(c));c[6]=0;c[11]=0;
    assert(hid_i2c_decode(&l,c,sizeof(c),&e)==1 && !e.contact && !e.buttons);
    c[6]=1;c[7]=0xff;c[8]=0xff;
    assert(hid_i2c_decode(&l,c,sizeof(c),&e)==-1);
    const uint8_t bad[]={0x75,32,0x96,0xff,0xff,0x81,2};
    assert(hid_i2c_parse_descriptor(&l,bad,sizeof(bad)));
    const uint8_t under[]={0xb4};assert(hid_i2c_parse_descriptor(&l,under,sizeof(under)));
    assert(hid_i2c_parse_descriptor(&l,touch,sizeof(touch)-1));
}
typedef struct {unsigned stage,emitted,pending,feature_set,releases,input_fail,feature_get_fail,reset_failures,reset_no_ack,feature_fill;const uint8_t *descriptor;size_t descriptor_bytes;uint8_t report[512];} Fake;
static const char *transfer(void *context,uint16_t address,const uint8_t *w,size_t wn,uint8_t *r,size_t rn){
    Fake *f=context;assert(address==0x2c);
    if(wn==2 && w[0]==0x20 && rn==30){
        const uint8_t descriptor[]={30,0,0,1,sizeof(mouse),0,0x21,0,0x22,0,7,0,0,0,0,0,0x23,0,0x24,0,0xcb,6,0xc6,0xcf,1,0,0,0,0,0};
        memcpy(r,descriptor,sizeof(descriptor));r[26]=0xa5; /* Upstream ignores reserved bytes. */
        if(f->descriptor){r[4]=(uint8_t)f->descriptor_bytes;r[5]=(uint8_t)(f->descriptor_bytes>>8);}
        f->stage=1;return 0;
    }
    if(wn==4){
        assert(w[0]==0x23 && w[1]==0);
        if(w[3]==1){if(f->reset_failures){f->reset_failures--;return "temporary reset NACK";}f->stage=2;}
        else assert(w[3]==8);return 0;
    }
    if(!wn && rn==2){f->stage=3;r[0]=f->reset_no_ack?7:0;r[1]=0;return 0;}
    if(!wn && rn==7){
        if(f->input_fail)return "temporary input error";
        memset(r,0,rn);
        memcpy(r,f->report,rn);return 0;
    }
    if(wn==2 && w[0]==0x21){
        assert(f->stage==3);const uint8_t *p=f->descriptor?f->descriptor:mouse;
        assert(rn==(f->descriptor?f->descriptor_bytes:sizeof(mouse)));memcpy(r,p,rn);return 0;
    }
    if(wn==6 && w[0]==0x23 && (w[2]&0xf0)==0x30 && w[3]==2 && w[4]==0x24 && rn==4){
        if(f->feature_get_fail)return "GET unsupported";
        r[0]=4;r[1]=0;r[2]=w[2]&15;r[3]=(uint8_t)f->feature_fill;return 0;
    }
    if(wn==10 && w[0]==0x23 && (w[2]&0xf0)==0x30 && w[3]==3 && w[4]==0x24){
        unsigned id=w[2]&15;
        assert(!rn && w[6]==4 && !w[7] && w[8]==id);
        unsigned fill=f->feature_get_fail?0:f->feature_fill;
        assert((id==4 && w[9]==3)||(id==6 && w[9]==((fill&~3u)|3))||
               (id==13 && w[9]==(fill&~1u)));
        f->feature_set++;return 0;
    }
    assert(!"unexpected HID transfer");return "unexpected";
}
static void delay(void *ctx,unsigned us){(void)ctx;(void)us;}
static int pending(void *ctx){return ((Fake *)ctx)->pending;}
static void emit(void *ctx,const HidI2cPointer *e){
    Fake *f=ctx;
    if(!e->contact && !e->buttons){f->releases++;return;}
    f->emitted++;assert(e->x==2 && e->y==-3 && e->buttons==1);
}
static void protocol(void){
    Fake f={.pending=1};HidI2c d;
    HidI2cIo io={&f,transfer,delay,pending};HidI2cConfig c={0x2c,0x20,0x06cb,0xcfc6};
    assert(!hid_i2c_init(&d,&c,&io));assert(d.ready && d.resets==1);
    const uint8_t report[]={7,0,1,1,2,0xfd,0};memcpy(f.report,report,7);
    assert(!hid_i2c_poll(&d,emit,&f));assert(f.emitted==1 && d.reports==1);
    f.pending=0;assert(!hid_i2c_poll(&d,emit,&f));assert(f.emitted==1);
    f.pending=1;f.report[0]=8;assert(!hid_i2c_poll(&d,emit,&f) && d.malformed==1 && d.ready);
    f.report[0]=0;assert(!hid_i2c_poll(&d,emit,&f) && d.ready && d.resets==2 && f.releases==1);
    f.input_fail=1;assert(!hid_i2c_poll(&d,emit,&f) && d.ready && d.io_errors==1);
    f.input_fail=0;f.report[0]=7;
    for(unsigned i=0;i<100;i++)assert(!hid_i2c_poll(&d,emit,&f));
    assert(!hid_i2c_poll(&d,emit,&f) && f.emitted==2 && d.ready);
    f.input_fail=1;
    for(unsigned attempt=0;attempt<3;attempt++){
        const char *error=hid_i2c_poll(&d,emit,&f);
        assert((error!=0)==(attempt==2));
        for(unsigned i=0;i<100;i++)assert(!hid_i2c_poll(&d,emit,&f));
    }
    assert(d.consecutive_errors==3 && d.io_errors==4);
    assert(!hid_i2c_sleep(&d));
}
static void captured_hp(void){
    uint8_t descriptor[4096];FILE *f=fopen("tools/x86_64/tests/fixtures/hid-i2c-syna32f6.bin","rb");
    assert(f);size_t n=fread(descriptor,1,sizeof(descriptor),f);assert(!ferror(f));fclose(f);
    assert(n==679);HidI2cLayout l;HidI2cPointer e;
    assert(!hid_i2c_parse_descriptor(&l,descriptor,n));
    assert(l.report_bits[0][2]==24 && l.report_bits[0][3]==232);
    assert(l.report_bits[2][4]==8);
    Fake transport={.pending=1,.descriptor=descriptor,.descriptor_bytes=n,.feature_fill=0xa0};HidI2c device;
    HidI2cIo io={&transport,transfer,delay,pending};HidI2cConfig c={0x2c,0x20,0x06cb,0xcfc6};
    assert(!hid_i2c_init(&device,&c,&io));assert(transport.feature_set==3 && device.ready);
    transport.feature_get_fail=1;transport.reset_failures=1;transport.reset_no_ack=1;
    assert(!hid_i2c_init(&device,&c,&io) && device.ready && device.reset_timeouts==1);
    assert(transport.feature_set==6);
    const uint8_t mouse[]={2,1,0xf9,0x0a};
    assert(hid_i2c_decode(&l,mouse,sizeof(mouse),&e)==1);
    assert(e.x==-7 && e.y==10 && e.buttons==1 && !e.absolute);
    uint8_t touchpad[30]={3,2,0x34,0x02,0x12,0x01};
    touchpad[28]=1;touchpad[29]=1;
    assert(hid_i2c_decode(&l,touchpad,sizeof(touchpad),&e)==1);
    assert(e.absolute && e.contact && e.x==0x234 && e.y==0x112);
    assert(e.x_min==0 && e.x_max==1572 && e.y_max==984 && e.buttons==1);
}
static void fuzz(void){
    uint32_t random=1;uint8_t b[128];HidI2cLayout l;HidI2cPointer e;
    for(unsigned n=0;n<30000;n++){
        random=random*1664525+1013904223;size_t size=1+(random%sizeof(b));
        for(size_t i=0;i<size;i++){random=random*1664525+1013904223;b[i]=(uint8_t)(random>>24);}
        if(!hid_i2c_parse_descriptor(&l,b,size))(void)hid_i2c_decode(&l,b,size,&e);
    }
}
int main(void){parsing();protocol();captured_hp();fuzz();puts("HID I2C pointer descriptor/report/protocol checks passed");}
