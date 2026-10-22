/* Native-only ABI. Buffers belong to the platform, never to C16 user memory. */
#ifndef RIPLIX_DESKTOP_CORE_H
#define RIPLIX_DESKTOP_CORE_H
#include <stdint.h>
#include <stddef.h>
void xd_init(int32_t,int32_t,void (*)(uint32_t,uint32_t,int32_t,int32_t,int32_t,int32_t));
void xd_add(uint32_t,uint32_t,uint32_t,uint32_t);
void xd_remove(uint32_t);
void xd_accept(uint32_t,uint32_t,uint32_t,uint32_t);
int32_t xd_command(uint32_t,uint32_t,int32_t,int32_t,uint32_t,uint32_t);
void xd_motion(int32_t,int32_t);
void xd_button(uint32_t,uint32_t);
void xd_edges(int32_t,int32_t,uint32_t);
uint32_t xd_focus(void);
uint32_t xd_hit(int32_t,int32_t);
uint32_t xd_captured(void);
uint32_t xd_window(uint32_t,int32_t[6]);
void xd_task(uint32_t,int32_t[4]);
void xd_invalidate(int32_t,int32_t,int32_t,int32_t);
uint32_t xd_damage(int32_t[128]);
void xd_chrome(uint32_t *,uint32_t,uint32_t,uint32_t,const uint8_t *,uint32_t,const int32_t[4]);
void xd_label(uint32_t *,uint32_t,uint32_t,int32_t,int32_t,int32_t,int32_t,const uint8_t *,uint32_t,const int32_t[4]);
struct xd_primitive {int32_t x0,y0,x1,y1;uint16_t dot,size;};
int32_t xd_a8(uint8_t *,size_t,size_t,size_t,const struct xd_primitive *,size_t,uint32_t);
#endif
