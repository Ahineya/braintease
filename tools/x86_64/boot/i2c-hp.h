/* Exact HP Envy 16-ac0xxx / board8C66 profile, captured from firmware and Linux.
 * Caller must check PCI8086:7e7a,subsystem103c:8c66 at00:15.2 and put it D0,
 * memory-decode enabled, before mapping BAR0. No DMA or external IRQ required.
 * BAR0 itself need not be page aligned: map its exact address +minimum span. */
#ifndef RIPLIX_I2C_HP_H
#define RIPLIX_I2C_HP_H
#include "i2c-designware.h"
#include "hid_i2c.h"
#define I2C_HP_CONTROLLER_BYTES 0x300u /* DW0..0xff, LPSSprivate0x200..0x2ff */
#define I2C_HP_GPIO_BASE UINT64_C(0xe0d50000)
#define I2C_HP_GPIO_BYTES 0x10000u
#define I2C_HP_PCI_DEVICE 0x7e7au
#define I2C_HP_PCI_SUBSYSTEM 0x8c66103cu
typedef struct {
    I2cDw controller;
    HidI2c hid;
    I2cDwIo gpio;
    unsigned ready, gpio_pad_offset, poll_gpio;
    uint32_t gpio_pad_raw;
} I2cHp;
/* gpio is optional. UsableGPIO provides a cheap pending check; otherwise
 * unconditional HID polling follows OpenBSD ihidev. GPIO writes never occur. */
const char *i2c_hp_init(I2cHp *,const I2cDwIo *controller,const I2cDwIo *gpio,uint64_t physical_bar);
const char *i2c_hp_poll(I2cHp *,HidI2cEmit,void *);
void i2c_hp_stop(I2cHp *);
#endif
