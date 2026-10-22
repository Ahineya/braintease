#ifndef RIPLIX_HID_I2C_H
#define RIPLIX_HID_I2C_H
#include <stddef.h>
#include <stdint.h>
#define HID_I2C_MAX_DESCRIPTOR 4096
#define HID_I2C_MAX_INPUT 512
#define HID_I2C_MAX_FIELDS 192
/* Native pointer transport contract: absolute coordinates retain descriptor
 * ranges; relative reports contain signed deltas. Button bits 0..4 map to
 * left/right/middle/back/forward. No guest addresses cross this boundary. */
typedef struct {
    int32_t x, y, x_min, x_max, y_min, y_max, wheel;
    uint32_t buttons;
    unsigned absolute, contact, contact_id;
} HidI2cPointer;
typedef void (*HidI2cEmit)(void *, const HidI2cPointer *);
typedef struct {
    uint16_t page, usage, offset, size;
    int32_t minimum, maximum;
    uint8_t report_id, type, relative, finger;
} HidI2cField;
typedef struct {
    HidI2cField fields[HID_I2C_MAX_FIELDS];
    unsigned count, numbered;
    uint8_t feature_unknown[256];
    uint16_t report_bits[3][256]; /* input/output/feature payload only */
} HidI2cLayout;
typedef struct {
    void *context;
    const char *(*transfer)(void *, uint16_t address, const uint8_t *write,
                           size_t write_bytes, uint8_t *read, size_t read_bytes);
    void (*delay_us)(void *, unsigned);
    /* GPIO level predicate: >0 report pending, 0 none, <0 inaccessible. Must
     * reflect the ACPI GPIO active polarity. Optional: bounded explicit poll. */
    int (*pending)(void *);
} HidI2cIo;
typedef struct {
    uint16_t address, descriptor_register, vendor, product;
} HidI2cConfig;
typedef struct {
    HidI2cIo io;
    HidI2cConfig config;
    HidI2cLayout layout;
    uint16_t report_register, input_register, max_input, command_register, data_register;
    uint8_t input[HID_I2C_MAX_INPUT];
    unsigned ready, retry_polls, powered, consecutive_errors;
    const char *feature_error;
    HidI2cPointer last_pointer;
    unsigned have_pointer;
    uint64_t reports, malformed, resets, io_errors, reset_timeouts, feature_errors;
} HidI2c;
const char *hid_i2c_parse_descriptor(HidI2cLayout *,const uint8_t *,size_t);
/* 1 = pointer event, 0 = other valid report, -1 = malformed. */
int hid_i2c_decode(const HidI2cLayout *,const uint8_t *,size_t,HidI2cPointer *);
const char *hid_i2c_init(HidI2c *,const HidI2cConfig *,const HidI2cIo *);
/* NULL success/no report. Does not busy-wait for a new user interaction. */
const char *hid_i2c_poll(HidI2c *,HidI2cEmit,void *);
const char *hid_i2c_sleep(HidI2c *);
#endif
