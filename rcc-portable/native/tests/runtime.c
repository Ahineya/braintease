/* Independent continuation oracle for generated QBE in native unit tests.
 * This models only the checked scalar ABI, not kernel scheduling or I/O. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uint64_t U;
struct frame { U token,pc,base,mark,result,args[64],slots[512]; unsigned initialized; };
struct context { struct frame frames[256]; unsigned depth; U stack,args[64]; uint16_t cells[6*64000]; };
static U calls,checkpoints,yields;
static struct frame *top(struct context *c) { if (!c->depth) abort(); return &c->frames[c->depth-1]; }
U rcc_native_process_pc(struct context *c) { return top(c)->pc; }
U rcc_native_process_set_pc(struct context *c,U pc) { top(c)->pc=pc;memset(top(c)->slots,0,sizeof top(c)->slots);return 0; }
U rcc_native_process_arg(struct context *c,U n) { if(n>=64)abort();return top(c)->args[n]; }
U rcc_native_process_slot(struct context *c,U n) { if(n>=512)abort();return top(c)->slots[n]; }
U rcc_native_process_save(struct context *c,U n,U v) { if(n>=512)abort();top(c)->slots[n]=v;return 0; }
U rcc_native_process_argument(struct context *c,U n,U v) { if(n>=64)abort();c->args[n]=v;return 0; }
U rcc_native_image_token(struct context *c,U token) { (void)c;return token; }
static U pointer(U at) { return ((at/64000)<<16)|(at%64000); }
static U linear(U p) { if((p&65535)>=64000 || p>>16>=6)abort();return (p>>16)*64000+(p&65535); }
U rcc_native_process_frame(struct context *c,U size) {
 struct frame *f=top(c);
 if(!f->initialized) { if(size>320000-c->stack)abort();f->mark=c->stack;f->base=pointer(c->stack);c->stack+=size;f->initialized=1; }
 return f->base;
}
U rcc_native_process_call(struct context *c,U token,U count,U result) {
 struct frame *f;
 if(c->depth==256 || count>64 || result>512)abort();
 f=&c->frames[c->depth++];memset(f,0,sizeof *f);f->token=token;f->result=result;memcpy(f->args,c->args,count*sizeof(U));calls++;return 0;
}
U rcc_native_process_return(struct context *c,U value) {
 struct frame *f=top(c);U result=f->result;
 c->stack=f->mark;c->depth--;if(c->depth && result)top(c)->slots[result-1]=value;
 if(!c->depth)c->args[0]=value;return 0;
}
U rcc_native_process_checkpoint(struct context *c) { (void)c;return ++checkpoints%3==0; }
U rcc_native_process_request(struct context *c,U op,U a,U b,U result) { (void)c;(void)a;(void)b;(void)result;if(op==12) { U at=linear(a); if(c->cells[at] || c->cells[at+9])abort(); c->args[0]=c->cells[at+1]; c->depth=0; return 0; } if(op!=15)abort();yields++;return 0; }
U rcc_native_process_bad_pc(struct context *c) { (void)c;abort(); }
U rcc_native_poll(struct context *c) { (void)c;return 0; }
U rcc_native_gep(struct context *c,U p,U delta) { (void)c;int64_t at=(int64_t)linear(p)+(int64_t)delta;if(at<0 || at>=6*64000)abort();return pointer((U)at); }
U rcc_native_load(struct context *c,U p,U width) { U at=linear(p),v=0;if(!width || width>4 || at+width>6*64000)abort();for(U n=0;n<width;n++)v|=(U)c->cells[at+n]<<(n*16);return v; }
U rcc_native_store(struct context *c,U p,U v,U width) { U at=linear(p);if(!width || width>4 || at+width>6*64000)abort();for(U n=0;n<width;n++)c->cells[at+n]=(uint16_t)(v>>(n*16));return 0; }
U probe_calls(void) { return calls; }
U probe_yields(void) { return yields; }
/* The Python test appends the function dispatch and driver here. */
