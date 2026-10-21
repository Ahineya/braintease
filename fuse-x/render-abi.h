/* SPDX-License-Identifier: GPL-2.0-only */
/* C16 envelope decoding only. Rendering and owned pixel storage live behind
 * the generic target/command contract; no native pointer crosses this file. */
static int render_geometry_zero(const struct gx_render *r) {
    return !r->width&&!r->height&&!r->format&&!r->x&&!r->y&&
           !r->dx&&!r->dy&&!r->stride;
}
static int render_data(const struct gx_render *r,int write) {
    if(r->data_length>GX_RENDER_MAX_DATA)return fail(EINVAL);
    if(!r->data_length)return r->data?fail(EINVAL):0;
    if(write)return valaddr_w(r->data,r->data_length,r->data_length)?0:-1;
    return valaddr_r(r->data,r->data_length,r->data_length)?0:-1;
}
static int render_commands(const struct gx_render *r,unsigned words,unsigned long limit) {
    unsigned long cells;
    if(r->count>limit)return fail(EINVAL);
    if(!r->count)return r->commands?fail(EINVAL):0;
    cells=r->count*words*sizeof(uint32_t);
    return valaddr_r((const uint8_t *)r->commands,cells,cells)?0:-1;
}
static int render_ioctl(char *user) {
    struct gx_render r;
    unsigned long a[12]={0};
    if(uget(user,&r,sizeof(r)))return -1;
    if(r.version!=GX_VERSION||r.operation>GX_RENDER_BLIT)return fail(EINVAL);
    if(r.operation==GX_RENDER_PROBE){
        if(r.window||r.target||!render_geometry_zero(&r)||r.flags||r.count||
            r.commands||r.data||r.data_length)return fail(EINVAL);
        return rcc_hosted_gx(30,a)<0?fail(ENOTTY):0;
    }
    if(!mine(r.window))return -1;
    a[0]=r.window;a[1]=r.operation;a[2]=r.target;
    if(r.operation==GX_RENDER_CREATE){
        if(r.target||!r.width||!r.height||r.width>GX_HOSTED_MAX_WIDTH*2||
            r.height>GX_HOSTED_MAX_HEIGHT*2||
            (r.format!=GX_XRGB8888&&r.format!=GX_RENDER_A8)||r.flags||
            r.x||r.y||r.dx||r.dy||r.stride||r.count||r.commands||r.data||r.data_length)
            return fail(EINVAL);
        /* A failed output address must not leak a newly allocated target. */
        if(!valaddr_w(user,sizeof(r),sizeof(r)))return -1;
        a[3]=r.width;a[4]=r.height;a[5]=r.format;
        if(host(30,a)<0)return -1;
        r.target=a[2];
        return uput(&r,user,sizeof(r));
    }
    if(!r.target)return fail(EINVAL);
    if(r.operation==GX_RENDER_DESTROY){
        if(!render_geometry_zero(&r)||r.flags||r.count||r.commands||r.data||r.data_length)
            return fail(EINVAL);
    }else if(r.operation==GX_RENDER_BATCH||r.operation==GX_RENDER_RASTER_A8){
        if(!render_geometry_zero(&r))return fail(EINVAL);
        if(r.operation==GX_RENDER_BATCH){
            if(r.flags)return fail(EINVAL);
            if(render_commands(&r,GX_RENDER_COMMAND_WORDS,GX_RENDER_MAX_COMMANDS)||
                render_data(&r,0))return -1;
        }else{
            if((r.flags&~GX_A8_CLEAR)||r.data||r.data_length)return fail(EINVAL);
            if(render_commands(&r,6,8192UL))return -1;
        }
        a[3]=r.flags;a[4]=r.count;a[5]=(unsigned long)r.commands;
        a[6]=(unsigned long)r.data;a[7]=r.data_length;
    }else if(r.operation==GX_RENDER_UPLOAD||r.operation==GX_RENDER_READBACK){
        if(!r.width||!r.height||r.format||r.flags||r.dx||r.dy||r.count||r.commands||
            !r.data_length)return fail(EINVAL);
        if(render_data(&r,r.operation==GX_RENDER_READBACK))return -1;
        a[3]=r.x;a[4]=r.y;a[5]=r.width;a[6]=r.height;
        a[7]=r.stride;a[8]=(unsigned long)r.data;a[9]=r.data_length;
    }else if(r.operation==GX_RENDER_BLIT){
        if(!r.width||!r.height||r.format||r.flags||r.count||r.stride||
            r.commands||r.data||r.data_length)return fail(EINVAL);
        a[3]=r.x;a[4]=r.y;a[5]=r.dx;a[6]=r.dy;a[7]=r.width;a[8]=r.height;
    }else return fail(EINVAL);
    return host(30,a)<0?-1:0;
}
