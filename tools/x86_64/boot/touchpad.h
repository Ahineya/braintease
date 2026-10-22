#ifndef RIPLIX_TOUCHPAD_H
#define RIPLIX_TOUCHPAD_H
#include <stdint.h>
void touchpad_init(void);
void touchpad_poll(void);
void touchpad_stop(void);
uint32_t touchpad_status(unsigned field);
const char *touchpad_error(void);
#endif
