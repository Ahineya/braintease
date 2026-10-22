/* An independent I2C input attachment must survive PS/2 initialization failure. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "../boot/bootinfo.h"
#include "../boot/keyboard.h"
#define RIPLIX_X86_KERNEL_H
#define BOOT_GRAPHICS 1
#define BOOT_TOUCHPAD 1
BootInfo boot={.tsc_hz=1000000};
static unsigned broken,pending,has_pending,configuration,attachments,polls;
static uint64_t ticks;
static uint64_t rdtsc(void){ticks+=1000;return ticks;}
static uint8_t in8(unsigned p){if(p==0x64)return broken?2:has_pending;if(p==0x60){has_pending=0;return pending;}return 0;}
static void out8(unsigned p,uint8_t value){
    if(p==0x64){if(value==0x20){pending=0;has_pending=1;}else if(value==0x60)configuration=1;}
    if(p==0x60){if(configuration){configuration=0;return;}pending=0xfa;has_pending=1;}
}
static void serial(const char *s){(void)s;}
static void console_flush(void){}
void x64_keyboard_init(X64Keyboard *k){memset(k,0,sizeof(*k));}
void x64_keyboard_events(X64Keyboard *k,void (*event)(void*,unsigned,unsigned),void *p){(void)k;(void)event;(void)p;}
void x64_keyboard_resync(X64Keyboard *k){(void)k;}
void x64_keyboard_feed(X64Keyboard *k,uint8_t scan){(void)k;(void)scan;}
int x64_keyboard_get(X64Keyboard *k){(void)k;return -1;}
void touchpad_init(void){attachments++;}
void touchpad_poll(void){polls++;}
int gx_active(void){return 0;}
void gx_key(unsigned usage,int down,unsigned modifiers){(void)usage;(void)down;(void)modifiers;}
#include "../boot/input.c"
int main(void){
    broken=1;input_init();assert(!keyboard_ready && attachments==1);
    input_poll();assert(polls==1);
    broken=0;input_init();assert(keyboard_ready && attachments==2);
    input_poll();assert(polls==2);
    puts("Input attachment checks passed: I2C touchpad initializes and polls with working or unavailable PS/2 keyboard");
}
