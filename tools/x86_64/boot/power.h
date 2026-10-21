#ifndef RIPLIX_X64_POWER_H
#define RIPLIX_X64_POWER_H
#include <stdint.h>
#include <stddef.h>
typedef struct {uint16_t port;uint8_t value,valid;} PowerReset;
PowerReset power_reset_parse(const uint8_t *,size_t);
#endif
