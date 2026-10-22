/* Native GX transport, independent of the C16 envelope and GOP pixel layout. */
#ifndef RIPLIX_X64_GX_H
#define RIPLIX_X64_GX_H
#include <stdint.h>
#include <stddef.h>
typedef struct {
    void *opaque;
    /* Complete canonical cell span or NULL; write marks the span dirty. */
    uint16_t *(*span)(void *opaque,uint32_t pointer,size_t cells,int write);
} GxGuest;
typedef struct {
    unsigned width,height;
    void *(*allocate)(size_t bytes);
    void (*release)(void *memory,size_t bytes);
    /* Source pixels are opaque 0x00RRGGBB. Borrowed synchronously, never retained. */
    void (*present)(const uint32_t *pixels,unsigned width,unsigned height);
    void (*restore_console)(void);
} GxPlatform;
void gx_bind(const GxPlatform *platform);
int32_t gx_call(uint64_t owner,unsigned operation,uint32_t arguments[12],const GxGuest *guest);
void gx_release(uint64_t owner);
int gx_active(void);
void gx_key(unsigned usage,int down,unsigned modifiers);
/* Coordinates in visible content pixels; wheel is signed 16.16 lines. */
void gx_pointer(int32_t x,int32_t y,uint32_t buttons,int32_t wheel);
#endif
