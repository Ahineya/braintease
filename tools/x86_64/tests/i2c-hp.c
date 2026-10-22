#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../boot/i2c-hp.h"
/* Validate exact-platform guards without host MMIO/PCI or duplicate transport
 * fixtures. Production I2C/HID implementations have their separate wire tests. */
static I2cDwTiming selected;
static unsigned stopped;
const char *i2c_dw_init(I2cDw *d,const I2cDwIo *io,const I2cDwTiming *t){d->io=*io;selected=*t;return 0;}
const char *i2c_dw_transfer(I2cDw *d,uint16_t a,const uint8_t *w,size_t wn,uint8_t *r,size_t rn){
    (void)d;(void)a;(void)w;(void)wn;(void)r;(void)rn;return 0;
}
void i2c_dw_stop(I2cDw *d){(void)d;stopped++;}
const char *hid_i2c_init(HidI2c *d,const HidI2cConfig *c,const HidI2cIo *io){
    assert(c->address==0x2c && c->descriptor_register==0x20 && c->vendor==0x06cb && c->product==0xcfc6);
    assert(io->pending(io->context)>=0);d->io=*io;return 0;
}
const char *hid_i2c_poll(HidI2c *d,HidI2cEmit emit,void *c){(void)d;(void)emit;(void)c;return 0;}
const char *hid_i2c_sleep(HidI2c *d){(void)d;return 0;}
typedef struct {uint32_t reg[512],offset[32],value[32];unsigned writes;} Fake;
static uint32_t read32(void *p,unsigned o){assert(o<sizeof(((Fake *)p)->reg));return ((Fake *)p)->reg[o/4];}
static void write32(void *p,unsigned o,uint32_t v){assert((o==0x204 && (v==0 || v==7)) || o==0x240 || o==0x244);Fake *f=p;f->reg[o/4]=v;assert(f->writes<32);f->offset[f->writes]=o;f->value[f->writes++]=v;}
static void delay(void *p,unsigned us){(void)p;assert(us==10000);}
int main(void){
    Fake c={0},g={0};I2cHp d;
    I2cDwIo io={&c,read32,write32,delay},gpio={&g,read32,0,0};
    c.reg[0x2fc/4]=0x10;g.reg[0xc/4]=0x600;g.reg[0x600/4]=0x80800102;c.reg[0xfc/4]=0x44570140;
    assert(i2c_hp_init(&d,&io,&gpio,UINT64_C(0x48192ce000)) && !c.writes);
    c.reg[0x2fc/4]=0;g.reg[0xc/4]=0x700;g.reg[0x700/4]=0x80800102;
    assert(!i2c_hp_init(&d,&io,&gpio,UINT64_C(0x48192ce000)) && d.poll_gpio && d.gpio_pad_offset==0x700);
    assert(c.writes==4 && c.reg[0x240/4]==0x192ce000 && c.reg[0x244/4]==0x48);
    assert(c.offset[0]==0x204 && !c.value[0] && c.offset[1]==0x204 && c.value[1]==7);
    assert(c.offset[2]==0x240 && c.offset[3]==0x244);
    g.reg[0x700/4]|=1u<<13;
    assert(!i2c_hp_init(&d,&io,&gpio,UINT64_C(0x48192ce000)) && !d.poll_gpio);
    assert(!i2c_hp_init(&d,&io,0,UINT64_C(0x48192ce000)) && !d.poll_gpio);
    assert(selected.high_count==100 && selected.low_count==200 && selected.sda_hold==0x10006 && selected.speed==2);
    i2c_hp_stop(&d);assert(stopped==1 && !d.ready && !g.writes);
    puts("HP LPSS/GPIO identity guards and firmware timing checks passed");
}
