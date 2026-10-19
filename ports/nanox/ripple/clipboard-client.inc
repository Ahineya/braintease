/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "clipboard-wire.h"
static int clipboard_request(struct nxr_clip_req *q, const char *text, struct nxr_clip_reply *reply) {
    struct nxr_clip_req *wire; nxReq header;
    wire=nxAllocReq(NXR_CLIPBOARD,sizeof(*wire),q->count);
    header=wire->header;*wire=*q;wire->header=header;wire->version=1;
    if(q->count)memcpy(wire+1,text,q->count);
    return TypedReadBlock(reply,sizeof(*reply),NXR_CLIPBOARD)==-1||reply->status?-1:0;
}
char *nx_clipboard_get(GR_WINDOW_ID window,size_t *length) {
    struct nxr_clip_req q;struct nxr_clip_reply reply;
    char *text=NULL;size_t total=0,at=0;
    *length=0;memset(&q,0,sizeof(q));q.window=window;q.op=NXR_CLIP_GET;
    LOCK(&nxGlobalLock);
    if(clipboard_request(&q,NULL,&reply)<0||reply.length>NXR_CLIPBOARD_MAX)goto done;
    total=reply.length;text=malloc(total+1);if(!text)goto done;
    q.op=NXR_CLIP_READ;
    while(at<total) {
        if(clipboard_request(&q,NULL,&reply)<0||!reply.count||reply.count>NXR_CLIPBOARD_CHUNK||reply.count>total-at) {
            free(text);text=NULL;goto done;
        }
        memcpy(text+at,reply.text,reply.count);at+=reply.count;
    }
    text[total]=0;*length=total;
done:
    q.op=NXR_CLIP_END;clipboard_request(&q,NULL,&reply);
    UNLOCK(&nxGlobalLock);return text;
}
int nx_clipboard_set(GR_WINDOW_ID window,const char *text,size_t length) {
    struct nxr_clip_req q;struct nxr_clip_reply reply;size_t at=0;int result=-1;
    if(length>NXR_CLIPBOARD_MAX||(!text&&length))return -1;
    memset(&q,0,sizeof(q));q.window=window;q.op=NXR_CLIP_SET;q.length=length;
    LOCK(&nxGlobalLock);
    if(clipboard_request(&q,NULL,&reply)<0)goto done;
    q.op=NXR_CLIP_WRITE;
    while(at<length) {
        q.count=length-at>NXR_CLIPBOARD_CHUNK?NXR_CLIPBOARD_CHUNK:length-at;
        if(clipboard_request(&q,text+at,&reply)<0)goto done;
        at+=q.count;
    }
    q.op=NXR_CLIP_COMMIT;q.count=0;result=clipboard_request(&q,NULL,&reply);
done:
    q.op=NXR_CLIP_END;q.count=0;clipboard_request(&q,NULL,&reply);
    UNLOCK(&nxGlobalLock);return result;
}
