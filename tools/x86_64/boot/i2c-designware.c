/* Register definitions and polling ordering reviewed against OpenBSD dwiic.c /
 * dwiicreg.h and Linux i2c-designware-master.c. See input/README.md. */
#include "i2c-designware.h"
#define CON 0x00
#define TAR 0x04
#define DATA 0x10
#define SS_HIGH 0x14
#define SS_LOW 0x18
#define FS_HIGH 0x1c
#define FS_LOW 0x20
#define MASK 0x30
#define RAW 0x34
#define RX_TL 0x38
#define TX_TL 0x3c
#define CLEAR 0x40
#define CLEAR_ABORT 0x54
#define CLEAR_STOP 0x60
#define ENABLE 0x6c
#define STATUS 0x70
#define TX_LEVEL 0x74
#define RX_LEVEL 0x78
#define SDA_HOLD 0x7c
#define ABORT_SOURCE 0x80
#define ENABLE_STATUS 0x9c
#define PARAM 0xf4
#define VERSION 0xf8
#define TYPE 0xfc
#define ABORT 0x40
#define STOP 0x200
#define MAX_BYTES 4096
#define BUDGET_US 100000
static uint32_t rd(I2cDw *d,unsigned r) { return d->io.read32(d->io.context,r); }
static void wr(I2cDw *d,unsigned r,uint32_t v) { d->io.write32(d->io.context,r,v); }
static void delay(I2cDw *d,unsigned us) { d->io.delay_us(d->io.context,us); }
static int enable(I2cDw *d,unsigned on) {
    if(on) {
        wr(d,ENABLE,1); (void)rd(d,ENABLE_STATUS); /* Linux dummy-read quirk. */
        return 1; /* ENABLE_STATUS may be unimplemented; transfer proves it. */
    }
    /* Linux __i2c_dw_disable aborts a transfer holding SCL on an empty TX
     * FIFO before disabling. Plain ENABLE=0 can otherwise never complete. */
    if((rd(d,RAW)&(1u<<13)) || (rd(d,STATUS)&(1u<<7))) {
        if(!(rd(d,ENABLE)&1)){wr(d,ENABLE,1);delay(d,25);}
        wr(d,ENABLE,rd(d,ENABLE)|2);
        unsigned n;
        for(n=0;n<10;n++){if(!(rd(d,ENABLE)&2))break;delay(d,10);}
        if(n==10)return 0;
    }
    for(unsigned n=0;n<100;n++) {
        wr(d,ENABLE,0);
        if(!(rd(d,ENABLE_STATUS)&1))return 1;
        delay(d,25);
    }
    return 0;
}
void i2c_dw_stop(I2cDw *d) {
    if(!d || !d->io.write32 || !d->io.read32 || !d->io.delay_us)return;
    wr(d,MASK,0); (void)enable(d,0); (void)rd(d,CLEAR);
    d->ready=0; d->busy=0;
}
const char *i2c_dw_init(I2cDw *d,const I2cDwIo *io,const I2cDwTiming *t) {
    if(!d || !io || !io->read32 || !io->write32 || !io->delay_us || !t ||
       !t->high_count || !t->low_count || (t->speed!=1 && t->speed!=2))
        return "I2C resource/timing missing";
    *d=(I2cDw){.io=*io};
    if(rd(d,TYPE)!=0x44570140)return "I2C component identity";
    if(!enable(d,0))return "I2C disable timeout";
    uint32_t p=rd(d,PARAM);
    d->tx_depth=((p>>16)&255)+1;
    d->rx_depth=((p>>8)&255)+1;
    wr(d,MASK,0); (void)rd(d,CLEAR);
    wr(d,t->speed==1?SS_HIGH:FS_HIGH,t->high_count);
    wr(d,t->speed==1?SS_LOW:FS_LOW,t->low_count);
    if(rd(d,VERSION)>=0x3131312a)wr(d,SDA_HOLD,t->sda_hold);
    wr(d,RX_TL,0); wr(d,TX_TL,0);
    wr(d,CON,(rd(d,CON)&(1u<<11))|1|0x20|0x40|(t->speed<<1));
    d->ready=1; return 0;
}
const char *i2c_dw_transfer(I2cDw *d,uint16_t address,const uint8_t *w,size_t wn,
                           uint8_t *r,size_t rn) {
    if(!d || !d->ready)return "I2C controller not initialized";
    if(d->busy)return "I2C transfer busy";
    if(address<8 || address>0x77 || (!wn && !rn) || wn>MAX_BYTES || rn>MAX_BYTES ||
       (wn && !w) || (rn && !r))return "I2C transfer bounds";
    d->busy=1; d->abort_source=0;
    const char *error=0;
    unsigned wait=0;
    while(rd(d,STATUS)&1) {
        if(wait++>=BUDGET_US/10){error="I2C bus busy timeout";goto out;}
        delay(d,10);
    }
    if(!enable(d,0)){error="I2C disable timeout";goto out;}
    wr(d,TAR,address); wr(d,MASK,0); (void)rd(d,CLEAR);
    if(!enable(d,1)){error="I2C enable timeout";goto out;}
    size_t sent=0, requested=0, received=0;
    unsigned budget=(wn+rn>1024)?1000000:BUDGET_US;
    for(wait=0;wait<budget/10;wait++) {
        uint32_t raw=rd(d,RAW);
        if(raw&ABORT) {
            d->abort_source=rd(d,ABORT_SOURCE); (void)rd(d,CLEAR_ABORT);
            error="I2C transfer aborted";goto out;
        }
        if(raw&(2|8)){error="I2C FIFO overflow";goto out;}
        unsigned available=rd(d,RX_LEVEL);
        if(available>rn-received || available>requested-received){error="I2C unexpected receive bytes";goto out;}
        while(available--)r[received++]=(uint8_t)rd(d,DATA);
        unsigned level=rd(d,TX_LEVEL);
        if(level>d->tx_depth){error="I2C TX FIFO level";goto out;}
        unsigned room=d->tx_depth-level;
        while(room && sent<wn) {
            uint32_t v=w[sent++];
            if(sent==wn && !rn)v|=STOP;
            wr(d,DATA,v);room--;
        }
        /* Every outstanding read reserves one receive FIFO slot. The CPU
         * drains RX before issuing more, including transfers > FIFO depth. */
        while(room && sent==wn && requested<rn &&
              requested-received<d->rx_depth) {
            uint32_t v=0x100;
            if(!requested && wn)v|=0x400; /* repeated START on direction change */
            if(++requested==rn)v|=STOP;
            wr(d,DATA,v);room--;
        }
        if((raw&STOP) && sent==wn && received==rn) {
            (void)rd(d,CLEAR_STOP);
            /* Linux xfer_common: STOP_DET can precede MASTER_ACTIVITY clear.
             * Disabling immediately can strand SCL low (~1:500 upstream). */
            for(unsigned idle=0;rd(d,STATUS)&(1u<<5);idle++) {
                if(idle==2000){error="I2C master active after STOP";goto out;}
                delay(d,10);
            }
            goto out;
        }
        delay(d,10);
    }
    error="I2C transfer timeout";
out:
    if(!enable(d,0) && !error)error="I2C transfer disable timeout";
    (void)rd(d,CLEAR);d->busy=0;
    return error;
}
