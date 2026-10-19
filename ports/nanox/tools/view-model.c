/* SPDX-License-Identifier: MIT */
#include <assert.h>
#include <stdio.h>
#include "../ripple/nxview-model.h"
int main(void)
{
    struct image_view v={0};int i,j,k;
    v.iw=4096;v.ih=64;v.vw=1;v.vh=1;v.fit=1;view_layout(&v);
    assert(v.dw==1&&v.dh==1);
    v.iw=1200;v.ih=800;v.vw=600;v.vh=400;view_layout(&v);
    assert(v.dw==600&&v.dh==400&&!v.px&&!v.py);
    view_zoom(&v,1);assert(v.dw==756&&v.dh==504&&v.px==78&&v.py==52);
    v.zoom=100;v.fit=0;v.px=v.py=32000;view_layout(&v);
    assert(v.dw==1200&&v.dh==800&&v.px==600&&v.py==400);
    for(i=1;i<=4096;i=i*2+1)for(j=1;j<=4096;j=j*2+1){
        v.iw=i;v.ih=j;v.vw=321;v.vh=217;v.fit=1;view_layout(&v);
        assert(v.dw>=1&&v.dw<=321&&v.dh>=1&&v.dh<=217);
        for(k=0;k<80;k++){
            view_zoom(&v,k<40?1:-1);
            assert(v.dw>0&&v.dw<=16384&&v.dh>0&&v.dh<=16384);
            assert(v.px>=0&&v.py>=0);
            assert(v.px<=(v.dw>v.vw?v.dw-v.vw:0));
            assert(v.py<=(v.dh>v.vh?v.dh-v.vh:0));
        }
        v.vw=1920;v.vh=1028;view_layout(&v);
        assert(v.px<=(v.dw>v.vw?v.dw-v.vw:0));
    }
    puts("Image viewer: fit, zoom anchoring, resize and C16 coordinate bounds passed");return 0;
}
