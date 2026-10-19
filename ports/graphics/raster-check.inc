/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Shared acceptance probe: bank boundaries, strides, rejection and fork COW. */
static int raster_checks(int descriptor){
    struct gx_raster r;unsigned short *a,*b;unsigned long i;int pid,status;
#define R(x) do{if(!(x)){fprintf(stderr,"raster check failed line %u errno %d\n",__LINE__,errno);return 1;}}while(0)
    a=malloc(65538UL);b=malloc(65538UL);R(a&&b);
    a[0]=a[65537UL]=b[0]=b[65537UL]=0x5aa5;
    memset(&r,0,sizeof(r));r.version=GX_VERSION;r.width=512;r.height=128;r.dst_stride=512;r.colour=0x35e7;r.dst=a+1;
    R(ioctl(descriptor,GX_RASTER_FILL,&r)==0);
    for(i=1;i<=65536UL;i++)R(a[i]==0x35e7);
    r.dst=b+1;r.src=a+1;r.src_stride=512;R(ioctl(descriptor,GX_RASTER_COPY,&r)==0);
    for(i=1;i<=65536UL;i++)R(b[i]==0x35e7);
    r.width=257;r.height=31;r.colour=0xf81f;R(ioctl(descriptor,GX_RASTER_FILL,&r)==0);
    for(i=0;i<31UL*512;i++)R(b[i+1]==(i%512<257?0xf81f:0x35e7));
    r.src=b+1;R(ioctl(descriptor,GX_RASTER_COPY,&r)<0&&errno==EINVAL);
    r.src=(void *)0;R(ioctl(descriptor,GX_RASTER_COPY,&r)<0&&errno==EFAULT);
    r.dst_stride=256;R(ioctl(descriptor,GX_RASTER_FILL,&r)<0&&errno==EINVAL);
    R(a[0]==0x5aa5&&a[65537UL]==0x5aa5&&b[0]==0x5aa5&&b[65537UL]==0x5aa5);
    pid=fork();R(pid>=0);
    if(!pid){
        /* Resolve scattered COW pages out of order before the bulk operation:
         * the physical run must stop at each resulting discontinuity. */
        a[50000UL]=a[1024]=a[32000UL]=0x4321;b[2048]=b[48000UL]=0x5678;
        r.dst=a+1;r.dst_stride=r.src_stride=r.width=512;r.height=128;r.colour=0x1234;
        if(ioctl(descriptor,GX_RASTER_FILL,&r))_exit(1);
        for(i=1;i<=65536UL;i++)if(a[i]!=0x1234)_exit(2);
        r.dst=b+1;r.src=a+1;if(ioctl(descriptor,GX_RASTER_COPY,&r))_exit(3);
        for(i=1;i<=65536UL;i++)if(b[i]!=0x1234)_exit(4);
        _exit(a[0]==0x5aa5&&a[65537UL]==0x5aa5&&b[0]==0x5aa5&&b[65537UL]==0x5aa5?0:5);
    }
    R(waitpid(pid,&status,0)==pid&&status==0);R(a[1]==0x35e7&&a[50000UL]==0x35e7&&b[1]==0xf81f);
    free(a);free(b);return 0;
#undef R
}
