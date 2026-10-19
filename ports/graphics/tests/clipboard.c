/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "../terminal-input.h"
#include "../terminal-selection.h"
#include "../../netsurf/frontend/address.h"
#include <assert.h>
#include <stdio.h>
static void addresses(void) {
    static const char *cases[][2]={
        {"example.com","https://example.com"},{"  wiby.me/path?q=yes  ","https://wiby.me/path?q=yes"},
        {"http://example.com","http://example.com"},{"HTTPS://example.com","HTTPS://example.com"},
        {"localhost:8080/a","https://localhost:8080/a"},{"127.0.0.1:80","https://127.0.0.1:80"},
        {"[::1]:443/a","https://[::1]:443/a"},{"//example.com/a","https://example.com/a"},
        {"/index.html","file:///index.html"},{"/","file:///"},{"file:///tmp/a","file:///tmp/a"},
        {"data:text/plain,hello","data:text/plain,hello"},{"about:blank","about:blank"}};
    char out[128];unsigned i;
    for(i=0;i<sizeof(cases)/sizeof(*cases);i++){assert(!browser_address(cases[i][0],out,sizeof(out)));assert(!strcmp(out,cases[i][1]));}
    assert(browser_address("",out,sizeof(out))<0&&browser_address(" \t",out,sizeof(out))<0);
    assert(browser_address("example.com\nevil.com",out,sizeof(out))<0);
    assert(browser_address("javascript:alert(1)",out,sizeof(out))<0);
    assert(browser_address("ftp://example.com",out,sizeof(out))<0);
    assert(browser_address("example.com",out,19)<0); /* Exact size without NUL. */
    assert(!browser_address("example.com",out,20));
}
int main(void) {
    struct vt_input q={0};struct vt t={0};struct vt_selection s={0};
    char source[32768],*copied;size_t length;unsigned i;unsigned long at=0;
    addresses();assert(vt_selection_at(&(struct vt){.cols=10,.rows=3},-4,99)==20);memset(source,'X',sizeof(source));
    assert(!vt_input_put(&q,"first",5));
    assert(!vt_input_paste(&q,source,sizeof(source),0));assert(!vt_input_put(&q,"last",4));
    while(q.length){unsigned n=q.length>7?7:q.length;
        for(i=0;i<n;i++,at++)assert(q.text[q.head+i]==(at<5?"first"[at]:at<32773?'X':"last"[at-32773]));
        vt_input_consume(&q,n);
    }
    assert(at==32777&&!q.head);
    assert(!vt_input_paste(&q,"a\r\nb\nc\rd\033\003\177\tZ",14,0));
    assert(q.length==9&&!memcmp(q.text,"a\rb\rc\rd\tZ",9));
    vt_input_consume(&q,q.length);
    assert(!vt_init_size(&t,10,3));vt_feed(&t,"\033[?2004h",8);assert(t.bracketed_paste);
    assert(!vt_input_paste(&q,"A\r\nB",4,t.bracketed_paste));
    assert(q.length==15&&!memcmp(q.text,"\033[200~A\nB\033[201~",15));
    vt_input_consume(&q,2);assert(!vt_input_put(&q,"!",1));assert(q.length==14&&!memcmp(q.text,"200~A\nB\033[201~!",14));
    length=q.length;assert(vt_input_paste(&q,source,32769,0)<0&&q.length==length);
    vt_feed(&t,"\033c",2);assert(!t.bracketed_paste);
    vt_feed(&t,"A\320\226e\314\201\344\270\255Z",11);
    s.active=1;s.anchor=0;s.end=9;copied=vt_selection_text(&t,&s,&length);
    assert(copied&&length==10&&!memcmp(copied,"A\320\226e\314\201\344\270\255Z",11));free(copied);
    s.anchor=4;s.end=4;copied=vt_selection_text(&t,&s,&length);assert(copied&&length==3&&!memcmp(copied,"\344\270\255",3));free(copied);
    assert(vt_selected(&s,4)&&!vt_selected(&s,5)&&!vt_selected(&s,3));
    assert(vt_selection_colour(&s,4,0x17)==0x71);
    vt_destroy(&t);free(q.text);puts("clipboard: URL normalization, ordered large/partial pastes, controls, bracketed mode and Unicode selection passed");return 0;
}
