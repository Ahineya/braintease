/* Profile evidence and upstream comparison: input/README.md. Platform PCI and
 * device mapping stay with the caller; no magic scanning or AML execution. */
#include "i2c-hp.h"
static const char *transfer(void *ctx,uint16_t addr,const uint8_t *w,size_t wn,uint8_t *r,size_t rn){
    return i2c_dw_transfer(&((I2cHp *)ctx)->controller,addr,w,wn,r,rn);
}
static void delay_us(void *ctx,unsigned us){
    I2cDwIo *io=&((I2cHp *)ctx)->controller.io;
    io->delay_us(io->context,us);
}
static int pending(void *ctx){
    I2cHp *d=ctx;
    if(!d->poll_gpio)return 1;
    uint32_t value=d->gpio.read32(d->gpio.context,d->gpio_pad_offset);
    d->gpio_pad_raw=value;
    /* GPP_B_0 is GPIO input, not native function; bit1 is raw RX state. IRQ
     * polarity is active-low from ACPI. No pin mux or ownership writes. */
    if(value==UINT32_MAX || (value&((15u<<10)|(1u<<9)))){d->poll_gpio=0;return 1;}
    return !(value&2);
}
const char *i2c_hp_init(I2cHp *d,const I2cDwIo *io,const I2cDwIo *gpio,uint64_t physical_bar){
    if(!d || !io || !io->read32 || !io->write32 || !io->delay_us || !physical_bar || (physical_bar&15))
        return "HP I2C mappings missing";
    *d=(I2cHp){0};
    if(gpio)d->gpio=*gpio;
    /* LPSS type is tested before its reset write; PCI identity/PM-D0 are the
     * caller's job. DW identity follows reset release, as OpenBSD does. Keep
     * the clock divider untouched, as Linux does for I2C. */
    uint32_t caps=io->read32(io->context,0x2fc);
    if(caps==UINT32_MAX || (caps&0xf0))return "HP LPSS controller type";
    /* A Linux-configured IRQ pin is not proof of native firmware pin mode.
     * GPIO is only an optimization: OpenBSD ihidev polls without an IRQ when
     * unavailable. Never change pin ownership/mux or require an IRQ setup. */
    if(gpio && gpio->read32){
        uint32_t pad=gpio->read32(gpio->context,0x0c);
        if(pad && !(pad&3) && pad<=I2C_HP_GPIO_BYTES-4){
            d->gpio_pad_offset=pad;
            d->gpio_pad_raw=gpio->read32(gpio->context,pad);
            if(d->gpio_pad_raw!=UINT32_MAX && !(d->gpio_pad_raw&((15u<<10)|(1u<<9))))d->poll_gpio=1;
        }
    }
    /* Linux intel_lpss_init_dev resets the block and then updates its remap
     * address. Essential when firmware left BAR0 unassigned or we moved it. */
    io->write32(io->context,0x204,0);
    io->write32(io->context,0x204,7);
    io->write32(io->context,0x240,(uint32_t)physical_bar);
    io->write32(io->context,0x244,(uint32_t)(physical_bar>>32));
    io->delay_us(io->context,10000);
    if(io->read32(io->context,0xfc)!=0x44570140)return "HP I2C component identity";
    /* Linux bxt_i2c_info for8086:7e7a uses133MHz and171/208ns fall,
     *42ns hold. Captured AML has no SSCN/FMCN override, so match Linux's
     *computed fast-mode100/200 counts and6TX/1RX hold, not reset defaults. */
    I2cDwTiming timing={100,200,0x10006,2};
    const char *e=i2c_dw_init(&d->controller,io,&timing);if(e)return e;
    HidI2cConfig config={0x2c,0x20,0x06cb,0xcfc6};
    HidI2cIo hidio={d,transfer,delay_us,pending};
    e=hid_i2c_init(&d->hid,&config,&hidio);
    if(e){if(d->hid.powered)(void)hid_i2c_sleep(&d->hid);i2c_dw_stop(&d->controller);return e;}
    d->ready=1;return 0;
}
const char *i2c_hp_poll(I2cHp *d,HidI2cEmit emit,void *ctx){
    if(!d || !d->ready)return "HP touchpad not initialized";
    const char *e=hid_i2c_poll(&d->hid,emit,ctx);
    if(e){d->ready=0;i2c_dw_stop(&d->controller);}
    return e;
}
void i2c_hp_stop(I2cHp *d){
    if(!d)return;
    if(d->hid.powered)(void)hid_i2c_sleep(&d->hid);
    i2c_dw_stop(&d->controller);d->ready=0;
}
