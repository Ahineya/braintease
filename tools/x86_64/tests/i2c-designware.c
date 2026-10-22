#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../boot/i2c-designware.h"
typedef struct {
    uint32_t regs[64],raw,commands[8192];
    uint8_t rx[256];unsigned head,count,writes,reads,ncommands,delays,fail_enable,stall,abort,stop_activity,aborted;
} Fake;
static uint32_t read32(void *context,unsigned offset){
    Fake *f=context;assert(offset<256 && !(offset&3));
    if(offset==0x34)return f->raw|(f->abort?0x40:0);
    if(offset==0x70 && (f->raw&0x200) && f->stop_activity){f->stop_activity--;return 0x20;}
    if(offset==0x6c && (f->regs[offset/4]&2)){f->regs[offset/4]&=~2u;f->raw&=~(1u<<13);f->regs[0x70/4]&=~(1u<<7);f->aborted++;}
    if(offset==0x78)return f->count;
    if(offset==0x74)return 0;
    if(offset==0x10){assert(f->count);uint8_t r=f->rx[f->head++%256];f->count--;return r;}
    if(offset==0x40){f->raw=0;return 0;}
    if(offset==0x60){return 0;} /* Keep STOP set while MASTER_ACTIVITY drains. */
    if(offset==0x54){f->abort=0;return 0;}
    return f->regs[offset/4];
}
static void write32(void *context,unsigned offset,uint32_t value){
    Fake *f=context;assert(offset<256 && !(offset&3));f->regs[offset/4]=value;
    if(offset==0x6c && !f->fail_enable)f->regs[0x9c/4]=value;
    if(offset!=0x10)return;
    assert(f->ncommands<8192);f->commands[f->ncommands++]=value;
    if(f->stall || f->fail_enable)return;
    if(value&0x100){
        assert(f->count<8);f->rx[(f->head+f->count++)%256]=(uint8_t)f->reads++;
    }else f->writes++;
    if(value&0x200)f->raw|=0x200;
}
static void delay_us(void *context,unsigned us){Fake *f=context;assert(us<=25);f->delays+=us;}
static void setup(Fake *f,I2cDw *d){
    memset(f,0,sizeof(*f));f->regs[0xfc/4]=0x44570140;
    f->regs[0xf8/4]=0x3131312a;f->regs[0xf4/4]=(7<<16)|(7<<8);
    I2cDwIo io={f,read32,write32,delay_us};I2cDwTiming timing={100,200,0x123,2};
    assert(!i2c_dw_init(d,&io,&timing));assert(d->tx_depth==8 && d->rx_depth==8);
    assert(f->regs[0x1c/4]==100 && f->regs[0x20/4]==200 && f->regs[0x7c/4]==0x123);
}
int main(void){
    Fake f;I2cDw d;setup(&f,&d);
    uint8_t command[]={0x20,0},r[513];
    assert(!i2c_dw_transfer(&d,0x2c,command,2,r,sizeof(r)));
    assert(f.writes==2 && f.reads==513 && f.ncommands==515 && !f.count);
    for(unsigned i=0;i<sizeof(r);i++)assert(r[i]==(uint8_t)i);
    assert(f.commands[0]==0x20 && f.commands[1]==0);
    assert(f.commands[2]==0x500 && f.commands[514]==0x300);
    assert(!f.regs[0x6c/4] && !d.busy);
    setup(&f,&d);assert(!i2c_dw_transfer(&d,0x2c,command,2,0,0));
    assert(f.commands[1]==0x200 && f.writes==2);
    setup(&f,&d);assert(!i2c_dw_transfer(&d,0x2c,0,0,r,1));assert(f.commands[0]==0x300);
    assert(i2c_dw_transfer(&d,0,0,0,r,1));assert(i2c_dw_transfer(&d,0x2c,0,0,r,4097));
    setup(&f,&d);f.abort=1;f.regs[0x80/4]=0x1000;
    assert(i2c_dw_transfer(&d,0x2c,0,0,r,1));assert(d.abort_source==0x1000 && !d.busy);
    setup(&f,&d);f.stall=1;
    assert(i2c_dw_transfer(&d,0x2c,0,0,r,1));assert(f.delays==100000 && !d.busy);
    setup(&f,&d);f.fail_enable=1;assert(i2c_dw_transfer(&d,0x2c,0,0,r,1));assert(!d.busy);
    i2c_dw_stop(&d);assert(!d.ready);
    setup(&f,&d);f.stop_activity=3;
    assert(!i2c_dw_transfer(&d,0x2c,0,0,r,1) && !f.stop_activity);
    setup(&f,&d);f.raw=1u<<13;f.regs[0x70/4]=1u<<7;
    i2c_dw_stop(&d);assert(f.aborted==1 && !d.ready);
    puts("DesignWare repeated-start/FIFO/abort/deadline checks passed");
}
