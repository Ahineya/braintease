/* SPDX-License-Identifier: GPL-2.0-only */
/* Included by both kernel backends; bounds are checked before any write. */
static int raster_request(struct gx_raster *r,unsigned copy) {
    unsigned long dest_count,src_count,d,s;
    if(r->version!=GX_VERSION||!r->width||!r->height||
#ifdef CONFIG_FUSE_X
       r->width>GX_HOSTED_MAX_WIDTH||r->height>GX_HOSTED_MAX_HEIGHT||
#else
       r->width>1024||r->height>768||
#endif
       r->dst_stride<r->width||(copy&&r->src_stride<r->width))return fail(EINVAL);
    dest_count=(unsigned long)(r->height-1)*r->dst_stride+r->width;
    src_count=(unsigned long)(r->height-1)*r->src_stride+r->width;
    if(!valaddr_w(r->dst,dest_count,dest_count))return -1;
    if(copy){
        if(!valaddr_r(r->src,src_count,src_count))return -1;
        d=(unsigned long)r->dst;s=(unsigned long)r->src;
        d=(d>>16)*64000UL+(unsigned)d;s=(s>>16)*64000UL+(unsigned)s;
        if(d<s+src_count&&s<d+dest_count)return fail(EINVAL);
    }
    return 0;
}
