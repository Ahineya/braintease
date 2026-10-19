/* SPDX-License-Identifier: MIT */
#ifndef NXVIEW_MODEL_H
#define NXVIEW_MODEL_H
/* Coordinates stay representable by Nano-X's C16 signed coordinate type.
 * All products are widened before multiplication, including on native C16. */
struct image_view {
    int iw,ih,vw,vh,dw,dh,px,py,zoom,fit;
};
static int view_bound(long value,int maximum)
{
    return value<0?0:value>maximum?maximum:(int)value;
}
static void view_clamp(struct image_view *v)
{
    v->px=view_bound(v->px,v->dw>v->vw?v->dw-v->vw:0);
    v->py=view_bound(v->py,v->dh>v->vh?v->dh-v->vh:0);
}
static void view_layout(struct image_view *v)
{
    long limit;
    if(v->vw<1)v->vw=1;if(v->vh<1)v->vh=1;
    if(!v->iw||!v->ih){v->dw=v->dh=v->px=v->py=0;return;}
    if(v->fit){
        v->dw=v->iw;v->dh=v->ih;
        if(v->dw>v->vw){v->dw=v->vw;v->dh=(long)v->ih*v->dw/v->iw;}
        if(v->dh>v->vh){v->dh=v->vh;v->dw=(long)v->iw*v->dh/v->ih;}
        if(!v->dw)v->dw=1;if(!v->dh)v->dh=1;
        v->px=v->py=0;
    }else{
        limit=1638400L/(v->iw>v->ih?v->iw:v->ih);
        if(limit>800)limit=800;
        if(v->zoom<10)v->zoom=10;if(v->zoom>limit)v->zoom=limit;
        v->dw=(long)v->iw*v->zoom/100;v->dh=(long)v->ih*v->zoom/100;
        if(!v->dw)v->dw=1;if(!v->dh)v->dh=1;
        view_clamp(v);
    }
}
static void view_zoom(struct image_view *v,int direction)
{
    long cx,cy;int ow=v->dw,oh=v->dh,percent;
    if(!ow||!oh)return;
    cx=ow>v->vw?v->px+v->vw/2:ow/2;
    cy=oh>v->vh?v->py+v->vh/2:oh/2;
    percent=v->fit?(long)ow*100/v->iw:v->zoom;
    if(direction>0)percent+=percent/4+1;else percent-=percent/5+1;
    v->fit=0;v->zoom=percent;view_layout(v);
    v->px=view_bound(cx*v->dw/ow-v->vw/2,v->dw>v->vw?v->dw-v->vw:0);
    v->py=view_bound(cy*v->dh/oh-v->vh/2,v->dh>v->vh?v->dh-v->vh:0);
}
#endif
