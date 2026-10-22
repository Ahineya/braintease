/* Native platform adapter. C16 cells never become a framebuffer or DMA alias. */
#include "gx.h"
static uint32_t *graphics_front;
static unsigned graphics_width,graphics_height,graphics_bound,graphics_valid;
static int32_t graphics_mouse_x,graphics_mouse_y;
static unsigned graphics_mouse_valid;
static void graphics_cursor(unsigned restore);
static void *graphics_allocate(size_t bytes) {
    if (!bytes || bytes > SIZE_MAX-4095) return 0;
    return (void *)pages_alloc((bytes+4095)/4096);
}
static void graphics_free(void *memory,size_t bytes) {
    if (memory) pages_release(memory,bytes);
}
static void graphics_restore(void) {
    if (graphics_front) graphics_free(graphics_front,(size_t)graphics_width*graphics_height*4);
    graphics_front=0; graphics_width=graphics_height=graphics_valid=0;
    console_graphics(0);
}
static uint32_t graphics_pixel(uint32_t rgb) {
    if (boot.pixel_format) return rgb;
    return (rgb&0x00ff00U) | ((rgb>>16)&255) | ((rgb&255)<<16);
}
static void graphics_present(const uint32_t *pixels,unsigned width,unsigned height) {
    if (!width || !height || width>boot.width || height>boot.height) return;
    unsigned fresh=width!=graphics_width || height!=graphics_height || !graphics_valid;
    if (width!=graphics_width || height!=graphics_height) {
        if (graphics_front) graphics_free(graphics_front,(size_t)graphics_width*graphics_height*4);
        graphics_front=graphics_allocate((size_t)width*height*4);
        graphics_width=width;graphics_height=height;graphics_valid=0;
    }
    console_graphics(1);
    volatile uint32_t *video=(void *)boot.framebuffer;
    if (fresh) {
        for(unsigned y=0;y<boot.height;y++)
            for(unsigned x=0;x<boot.width;x++)video[(size_t)y*boot.stride+x]=0;
    }
    unsigned left=width,top=height,right=0,bottom=0;
    for(unsigned y=0;y<height;y++)for(unsigned x=0;x<width;x++) {
        size_t at=(size_t)y*width+x;
        if(fresh || !graphics_front || graphics_front[at]!=pixels[at]) {
            if(x<left)left=x;if(x+1>right)right=x+1;
            if(y<top)top=y;if(y+1>bottom)bottom=y+1;
        }
    }
    unsigned ox=(boot.width-width)/2,oy=(boot.height-height)/2;
    if(left<right && top<bottom)
        for(unsigned y=top;y<bottom;y++)for(unsigned x=left;x<right;x++)
            video[(size_t)(y+oy)*boot.stride+x+ox]=graphics_pixel(pixels[(size_t)y*width+x]);
    if(graphics_front)memcpy(graphics_front,pixels,(size_t)width*height*4);
    graphics_valid=1;
    if(graphics_mouse_valid)graphics_cursor(0);
    __asm__ volatile("sfence" : : : "memory");
}
static void graphics_cursor(unsigned restore) {
    if(!graphics_front || !gx_active())return;
    volatile uint32_t *video=(void *)boot.framebuffer;
    int32_t ox=(boot.width-graphics_width)/2,oy=(boot.height-graphics_height)/2;
    for(int32_t y=0;y<20;y++)for(int32_t x=0;x<12;x++) {
        int32_t sx=graphics_mouse_x+x,sy=graphics_mouse_y+y;
        if(sx<0 || sy<0 || (unsigned)sx>=boot.width || (unsigned)sy>=boot.height)continue;
        int triangle=y<16 && x<=y/2,stem=y>=10 && x>=4 && x<=6;
        if(!triangle && !stem)continue;
        uint32_t pixel=0;
        if(restore) {
            int32_t cx=sx-ox,cy=sy-oy;
            if(cx>=0 && cy>=0 && (unsigned)cx<graphics_width && (unsigned)cy<graphics_height)
                pixel=graphics_front[(size_t)cy*graphics_width+cx];
        } else pixel=(x==0 || (triangle && x==y/2) || (stem && (x==4 || x==6)) || y==19)?0:0xffffff;
        video[(size_t)sy*boot.stride+sx]=graphics_pixel(pixel);
    }
}
void graphics_pointer_report(int32_t x,int32_t y,uint32_t buttons,int32_t wheel) {
    if(graphics_mouse_valid)graphics_cursor(1);
    graphics_mouse_x=x;graphics_mouse_y=y;graphics_mouse_valid=1;
    if(gx_active()) {
        int32_t ox=(boot.width-graphics_width)/2,oy=(boot.height-graphics_height)/2;
        gx_pointer(x-ox,y-oy,buttons,wheel);
        graphics_cursor(0);
        __asm__ volatile("sfence" : : : "memory");
    }
}
static void graphics_ensure(void) {
    if(graphics_bound)return;
    GxPlatform p={boot.width,boot.height,graphics_allocate,graphics_free,graphics_present,graphics_restore};
    gx_bind(&p);graphics_bound=1;
}
static uint16_t *graphics_span(void *opaque,uint32_t p,size_t cells,int write) {
    Context *c=opaque;
    unsigned bank=p>>16,offset=p&65535;
    if(bank>=256 || offset>=BANK)return 0;
    size_t at=(size_t)bank*BANK+offset;
    if(at<32 || cells>CELLS-at)return 0;
    if(write)dirty(c,at,cells);
    return c->cells+at;
}
static U graphics_screen(Context *c,U op,U row,U destination,U capacity) {
    if(op==0)return boot.width;
    if(op==1)return boot.height;
    if(op!=2 || row>=boot.height || capacity<(U)boot.width*3 || destination>UINT32_MAX)return UINT32_MAX;
    uint16_t *out=graphics_span(c,(uint32_t)destination,(size_t)boot.width*3,1);
    if(!out)return UINT32_MAX;
    console_flush();
    const volatile uint32_t *video=(const void *)boot.framebuffer;
    for(unsigned x=0;x<boot.width;x++) {
        uint32_t pixel=video[(size_t)row*boot.stride+x];
        out[x*3]=boot.pixel_format?(pixel>>16)&255:pixel&255;
        out[x*3+1]=(pixel>>8)&255;
        out[x*3+2]=boot.pixel_format?pixel&255:(pixel>>16)&255;
    }
    return (U)boot.width*3;
}
/* Development-only request through the existing libc call carrier. */
static int graphics_syscall(Context *c,U request,U result) {
    if(request>UINT32_MAX)return 0;
    uint16_t *r=graphics_span(c,(uint32_t)request,10,1);
    if(!r || (r[0]!=0x7847 && r[0]!=0x7848))return 0;
    U op=r[1]|(uint32_t)r[2]<<16,row=r[3]|(uint32_t)r[4]<<16;
    U data=r[5]|(uint32_t)r[6]<<16,count=r[7]|(uint32_t)r[8]<<16;
    U value;
    if(r[0]==0x7847)value=graphics_screen(c,op,row,data,count);
    else {
#ifdef BOOT_TOUCHPAD
        if(op==6) {
            const char *message=touchpad_error();
            uint16_t *out=data<=UINT32_MAX && count && count<=1024 ? graphics_span(c,data,count,1):0;
            if(!out)value=UINT32_MAX;
            else {size_t n=0;while(message[n] && n+1<count){out[n]=(uint8_t)message[n];n++;}out[n]=0;value=n;}
        } else value=touchpad_status(op);
#else
        value=op?UINT32_MAX:0;
#endif
    }
    r[9]=value==UINT32_MAX?14:0;
    result_slot(c,result,value);return 1;
}
