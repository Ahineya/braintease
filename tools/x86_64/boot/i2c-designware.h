/* Native, bounded, polling DesignWare I2C master. PCI/ACPI resources belong to
 * platform discovery; this module never scans, guesses a BAR or touches DMA. */
#ifndef RIPLIX_I2C_DESIGNWARE_H
#define RIPLIX_I2C_DESIGNWARE_H
#include <stddef.h>
#include <stdint.h>
typedef struct {
    void *context;
    uint32_t (*read32)(void *, unsigned offset);
    void (*write32)(void *, unsigned offset, uint32_t value);
    void (*delay_us)(void *, unsigned microseconds);
} I2cDwIo;
typedef struct {
    uint16_t high_count, low_count;
    uint32_t sda_hold;
    unsigned speed; /* 1 = standard, 2 = fast; counts come from ACPI/firmware. */
} I2cDwTiming;
typedef struct {
    I2cDwIo io;
    unsigned tx_depth, rx_depth, ready, busy;
    uint32_t abort_source;
} I2cDw;
/* Returns NULL on success or a stable diagnostic. All waits have a deadline. */
const char *i2c_dw_init(I2cDw *, const I2cDwIo *, const I2cDwTiming *);
const char *i2c_dw_transfer(I2cDw *, uint16_t address, const uint8_t *write,
                          size_t write_bytes, uint8_t *read, size_t read_bytes);
void i2c_dw_stop(I2cDw *);
#endif
