/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Real extension, with only client/window lookup and the socket writer stubbed. */
#include <assert.h>
#include <stdio.h>
#include "../ripple/clipboard-server.c"
GR_CLIENT *curclient;
static GR_CLIENT owner,other;
static GR_WINDOW owned,foreign;
static struct nxr_clip_reply answer;
GR_CLIENT *GsFindClient(int fd){return fd==7?&owner:fd==8?&other:NULL;}
GR_WINDOW *GsFindWindow(GR_WINDOW_ID id){return id==2?&owned:id==3?&foreign:NULL;}
int nxr_write(int fd,const void *p,int n){(void)fd;if(n==sizeof(answer))memcpy(&answer,p,n);return 0;}
static struct {struct nxr_clip_req q;char data[NXR_CLIPBOARD_CHUNK];} packet;
static void call(unsigned op,unsigned long length,unsigned count) {
    packet.q.version=1;packet.q.op=op;packet.q.length=length;packet.q.count=count;packet.q.window=2;
    assert(nxr_clipboard_valid(&packet,sizeof(packet.q)+count));nxr_clipboard_dispatch(7,&packet);
}
int main(void) {
    unsigned i;unsigned long at;
    owned.id=2;owned.owner=&owner;foreign.id=3;foreign.owner=&other;
    call(NXR_CLIP_SET,32768,0);assert(!answer.status);
    memset(packet.data,'A',sizeof(packet.data));
    for(at=0;at<32768;at+=sizeof(packet.data)){call(NXR_CLIP_WRITE,0,sizeof(packet.data));assert(!answer.status);}
    call(NXR_CLIP_COMMIT,0,0);assert(!answer.status&&local_length==32768&&!transfers[7].text);
    call(NXR_CLIP_GET,0,0);assert(!answer.status&&answer.length==32768);
    /* A second client changes the live selection; this reader keeps its snapshot. */
    local_text[0]='B';
    for(at=0;at<32768;at+=sizeof(packet.data)) {
        call(NXR_CLIP_READ,0,0);assert(!answer.status&&answer.count==sizeof(packet.data));
        for(i=0;i<sizeof(packet.data);i++)assert(answer.text[i]=='A');
    }
    call(NXR_CLIP_END,0,0);assert(!transfers[7].text);
    call(NXR_CLIP_SET,3,0);packet.data[0]='X';call(NXR_CLIP_WRITE,0,1);
    call(NXR_CLIP_COMMIT,0,0);assert(answer.status&&local_text[0]=='B');
    nxr_clipboard_forget(7);assert(!transfers[7].text);
    call(NXR_CLIP_SET,1,0);packet.data[0]=0xff;call(NXR_CLIP_WRITE,0,1);
    call(NXR_CLIP_COMMIT,0,0);assert(answer.status&&local_length==32768);
    call(NXR_CLIP_SET,1,0);packet.data[0]=0;call(NXR_CLIP_WRITE,0,1);
    call(NXR_CLIP_COMMIT,0,0);assert(answer.status);
    call(NXR_CLIP_SET,4,0);memcpy(packet.data,"\360\237\231\202",4);call(NXR_CLIP_WRITE,0,4);
    call(NXR_CLIP_COMMIT,0,0);assert(!answer.status&&local_length==4);
    packet.q.op=NXR_CLIP_GET;packet.q.window=3;nxr_clipboard_dispatch(7,&packet);assert(answer.status);
    packet.q.window=2;packet.q.length=NXR_CLIPBOARD_MAX+1;assert(!nxr_clipboard_valid(&packet,sizeof(packet.q)));
    packet.q.length=0;packet.q.op=NXR_CLIP_WRITE;packet.q.count=NXR_CLIPBOARD_CHUNK+1;
    assert(!nxr_clipboard_valid(&packet,sizeof(packet)));
    call(NXR_CLIP_SET,0,0);call(NXR_CLIP_COMMIT,0,0);assert(!answer.status&&!local_length);
    nxr_clipboard_forget(7);free(local_text);
    puts("Nano-X clipboard: chunked snapshots, atomic commit, malformed/oversized text, ownership and disconnect cleanup passed");return 0;
}
