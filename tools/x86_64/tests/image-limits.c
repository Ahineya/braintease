/* Production RXE1 loader: full32MiB package, high native offsets/relocations,
 * failure rollback and complete page accounting. Never executes package code.
 * cc -O1 -Wall -Wextra -Werror -fsanitize=undefined \
 *   -fno-sanitize-recover=all tools/x86_64/tests/image-limits.c \
 *   -o /tmp/riplix-image-limits && /tmp/riplix-image-limits
 */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#undef memset
#undef memcpy
#undef memmove
#define memset test_memset
#define memcpy test_memcpy
#define memmove test_memmove
#define RIPLIX_X86_KERNEL_H
#include "../boot/bootinfo.h"
static BootInfo boot;
static uint64_t page_alloc(void), pages_alloc(unsigned), page_available(void);
static uint64_t page_mapping_owned(void) { return 0; }
static void page_free(uint64_t);
static void console_write(char c) { (void)c; }
static void console_boot_status(unsigned s,const char *m,const char *d) { (void)s;(void)m;(void)d; }
static void out8(uint16_t p,uint8_t v) { (void)p;(void)v;abort(); }
static int console_input(void) { return -1; }
static int console_dimensions(unsigned *rows,unsigned *columns) { *rows=24;*columns=80;return 1; }
static void screen_status(const char *s) { (void)s; }
static void serial(const char *s) { (void)s; }
static void serial_char(char c) { (void)c; }
static void hex(uint64_t n) { (void)n; }
static uint64_t rdtsc(void) { return 0; }
static uint64_t timer_ticks(void) { return 0; }
static void timer_idle(uint64_t ms) { (void)ms; }
static _Noreturn void fail(const char *s,uint64_t n) {
    fprintf(stderr,"%s: %llu\n",s,(unsigned long long)n); abort();
}
#include "../boot/runtime.c"
AcpiInfo acpi;
int entropy_available(void) { return 0; }
int entropy_fill(uint8_t *p,unsigned n) { (void)p;(void)n;abort(); }
U rcc_native_entry(Context *c) { (void)c;return 0; }
#define TEST_PAGES (40U * 1024 * 1024 / 4096)
static uint8_t arena[TEST_PAGES*4096] __attribute__((aligned(4096)));
static uint8_t busy[TEST_PAGES];
static unsigned fail_allocation;
static uint64_t page_available(void) {
    unsigned count=0;
    for(unsigned i=0;i<TEST_PAGES;i++)count+=!busy[i];
    return count;
}
static uint64_t pages_alloc(unsigned count) {
    if(fail_allocation)return 0;
    unsigned run=0;
    for(unsigned i=0;i<TEST_PAGES;i++) {
        if(busy[i]){run=0;continue;}
        if(++run!=count)continue;
        unsigned first=i+1-count;
        for(unsigned j=first;j<=i;j++)busy[j]=1;
        memset(arena+first*4096,0,(size_t)count*4096);
        return (uintptr_t)(arena+first*4096);
    }
    return 0;
}
static uint64_t page_alloc(void) { return pages_alloc(1); }
static void page_free(uint64_t address) {
    assert(address>=(uintptr_t)arena && address<(uintptr_t)(arena+sizeof(arena)) && !(address%4096));
    unsigned index=(address-(uintptr_t)arena)/4096;
    assert(busy[index]);busy[index]=0;
}
static Module *prepare(Context *c,size_t total) {
    assert(rcc_native_ext_rcc_hosted_image_begin(c,total));
    Process *p=c->processes[0];
    ImageHeader *h=(void *)p->loading;
    memcpy(h->magic,"FUSEX001",8);
    h->tag=0x31455852;h->total=total;h->functions=1;h->relocations=1;
    h->entry=1;h->global_bank=1;h->global_cells=16;
    h->code_size=total-sizeof(*h)-sizeof(ImageFunction)-sizeof(ImageRelocation);
    ImageFunction *f=(void *)(h+1);
    *f=(ImageFunction){1,1,h->code_size-1,0};
    ImageRelocation *r=(void *)(f+1);
    *r=(ImageRelocation){h->code_size-8,h->code_size-1};
    p->loading_done=total;
    assert(rcc_native_ext_rcc_hosted_image_prepare(c));
    U token=rcc_native_ext_rcc_hosted_image_poll(c);
    assert(token!=UINT32_MAX);
    Module *m=p->staged;
    assert(m && m->size==total);
    assert(c->functions[token_index(token)].address==(uintptr_t)m->code+h->code_size-1);
    assert(*(U *)(m->code+r->offset)==(uintptr_t)m->code+r->addend);
    return m;
}
int main(void) {
    assert(IMAGE_MAX==32U*1024*1024);
    Context *c=&context;
    c->stack=2*BANK+1;
    Process *p=c->processes[0]=process_allocate();assert(p);
    /* Keep one active image through both load failures and staged cleanup. */
    Module *active=prepare(c,4096);
    p->module=active;p->staged=NULL;
    U baseline=page_available();
    assert(!rcc_native_ext_rcc_hosted_image_begin(c,(U)IMAGE_MAX+1));
    assert(page_available()==baseline && p->module==active && !p->loading);
    fail_allocation=1;
    assert(!rcc_native_ext_rcc_hosted_image_begin(c,IMAGE_MAX));
    fail_allocation=0;
    assert(page_available()==baseline && p->module==active && !p->loading);
    Module *large=prepare(c,IMAGE_MAX);
    assert(large!=active && p->module==active);
    rcc_native_ext_rcc_hosted_image_cancel(c);
    assert(page_available()==baseline && p->module==active);
    /* A complete32MiB allocation still rejects headers beyond code/metadata
     * bounds, and its pages are reclaimed after the rejection. */
    assert(rcc_native_ext_rcc_hosted_image_begin(c,IMAGE_MAX));
    ImageHeader *h=(void *)p->loading;
    memcpy(h->magic,"FUSEX001",8);h->tag=0x31455852;h->total=IMAGE_MAX;
    h->code_size=IMAGE_MAX;h->functions=1;h->entry=1;h->global_bank=1;h->global_cells=16;
    p->loading_done=IMAGE_MAX;
    assert(rcc_native_ext_rcc_hosted_image_prepare(c));
    assert(rcc_native_ext_rcc_hosted_image_poll(c)==UINT32_MAX);
    rcc_native_ext_rcc_hosted_image_cancel(c);
    assert(page_available()==baseline && p->module==active);
    p->module=NULL;images_collect(c);process_release(p);c->processes[0]=NULL;
    assert(page_available()==TEST_PAGES);
    puts("image limit checks passed:32MiB total, high native offsets/relocations, oversize/allocation/metadata rejection, active image preservation and complete page reclamation");
}
