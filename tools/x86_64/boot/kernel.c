#include "kernel.h"
#include "acpi.h"
#ifdef BOOT_TOUCHPAD
#include "pci-refine.h"
#endif

BootInfo boot;
extern uint8_t kernel_stack_bottom[], kernel_stack_top[], kernel_stack_guard[], kernel_readonly_end[];
extern uint64_t isr_vectors[256];
extern void arch_load_gdt(const void *descriptor);
static uint64_t gdt[5];
static uint8_t double_fault_stack[16384] __attribute__((aligned(16)));
uint8_t irq_stack[16384] __attribute__((aligned(16)));
static struct __attribute__((packed)) {
    uint32_t reserved0;
    uint64_t rsp[3], reserved1, ist[7], reserved2;
    uint16_t reserved3, iomap;
} tss;
static struct __attribute__((packed)) Gate {
    uint16_t offset_low, selector;
    uint8_t ist, attributes;
    uint16_t offset_middle;
    uint32_t offset_high, reserved;
} idt[256] __attribute__((aligned(16)));
struct __attribute__((packed)) Table { uint16_t limit; uint64_t base; };
_Static_assert(sizeof(tss) == 104, "x86 TSS layout");
_Static_assert(sizeof(struct Gate) == 16, "x86 IDT gate layout");

static void tables(void) {
    gdt[1] = 0x00af9a000000ffffULL;
    gdt[2] = 0x00cf92000000ffffULL;
    tss.rsp[0] = (uint64_t)kernel_stack_top;
    tss.ist[0] = (uint64_t)(double_fault_stack + sizeof(double_fault_stack));
    tss.ist[1] = (uint64_t)(irq_stack + sizeof(irq_stack));
    tss.iomap = sizeof(tss);
    uint64_t base = (uint64_t)&tss;
    gdt[3] = (sizeof(tss) - 1) | ((base & 0xffffff) << 16) |
             (0x89ULL << 40) | ((base & 0xff000000) << 32);
    gdt[4] = base >> 32;
    const struct Table gdtr = {sizeof(gdt) - 1, (uint64_t)gdt};
    arch_load_gdt(&gdtr);
    for (unsigned vector = 0; vector < 256; ++vector) {
        uint64_t address = isr_vectors[vector];
        unsigned ist = vector == 8 ? 1 : (vector >= 32 && vector < 48) || vector == 255 ? 2 : 0;
        idt[vector] = (struct Gate){address, 8, ist, 0x8e,
                                  address >> 16, address >> 32, 0};
    }
    const struct Table idtr = {sizeof(idt) - 1, (uint64_t)idt};
    __asm__ volatile("lidt %0" : : "m"(idtr) : "memory");
    /* Mask devices until their controller and timer are initialized. */
    out8(0x21, 0xff); out8(0xa1, 0xff);
    serial("GDT_IDT_TSS_OK\n");
}

/* Identity mappings cover the first 64 GiB. Null and stack-guard pages are
 * absent. Device/framebuffer cache policy is installed separately below. */
static uint64_t pml4[512] __attribute__((aligned(4096)));
static uint64_t pdpt[512] __attribute__((aligned(4096)));
static uint64_t directories[64][512] __attribute__((aligned(4096)));
static uint64_t low_pages[512] __attribute__((aligned(4096)));
static uint64_t stack_pages[2][512] __attribute__((aligned(4096)));
static uint64_t readonly_pages[65][512] __attribute__((aligned(4096)));
static void paging(void) {
    uint64_t cr4;
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    if (cr4 & (1 << 12)) fail("five-level firmware paging is unsupported", cr4);
    pml4[0] = (uint64_t)pdpt | 3;
    for (unsigned gb = 0; gb < 64; ++gb) {
        pdpt[gb] = (uint64_t)directories[gb] | 3;
        for (unsigned mb = 0; mb < 512; ++mb)
            directories[gb][mb] = ((uint64_t)gb << 30) | ((uint64_t)mb << 21) | 0x83;
    }
    for (unsigned page = 1; page < 512; ++page) low_pages[page] = ((uint64_t)page << 12) | 3;
    directories[0][0] = (uint64_t)low_pages | 3;
    uint64_t guard = (uint64_t)kernel_stack_guard;
    uint64_t guard_end = (uint64_t)kernel_stack_bottom;
    unsigned stack_table = 0;
    for (uint64_t region = guard & ~0x1fffffULL; region < guard_end; region += 0x200000) {
        if (stack_table == 2) fail("stack guard table capacity", guard_end);
        uint64_t *pages = stack_pages[stack_table++];
        for (unsigned page = 0; page < 512; ++page) {
            uint64_t address = region + (uint64_t)page * 4096;
            if (address < guard || address >= guard_end) pages[page] = address | 3;
        }
        directories[region >> 30][(region >> 21) & 511] = (uintptr_t)pages | 3;
    }
    /* Keep relocated kernel instructions/constants immutable. A bad native
     * write must fault at its source instead of corrupting a later loader. */
    uint64_t first = boot.kernel_start, last = (uint64_t)kernel_readonly_end;
    if (first % 4096 || last <= first || last > boot.kernel_end || last - first > 128ULL*1024*1024)
        fail("kernel readonly span", last);
    unsigned table = 0;
    for (uint64_t at = first; at < last; at += 4096) {
        uint64_t *entry = &directories[at >> 30][(at >> 21) & 511];
        if (*entry & 128) {
            if (table == 65) fail("kernel readonly tables", table);
            uint64_t *pages = readonly_pages[table++];
            uint64_t base = at & ~0x1fffffULL;
            for (unsigned i = 0; i < 512; ++i) pages[i] = (base + i*4096ULL) | 3;
            *entry = (uintptr_t)pages | 3;
        }
        uint64_t *pages = (void *)(*entry & ~4095ULL);
        pages[(at >> 12) & 511] &= ~2ULL;
    }
    uint64_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 1ULL << 16; /* Enforce readonly pages in supervisor mode too. */
    __asm__ volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
    /* Drop firmware global translations too, including any huge mapping
     * covering our guard page. A CR3 reload alone need not invalidate them. */
    uint64_t without_global = cr4 & ~(1ULL << 7);
    __asm__ volatile("mov %0, %%cr4; mov %1, %%cr3; mov %2, %%cr4"
                     : : "r"(without_global), "r"((uint64_t)pml4), "r"(cr4) : "memory");
    uint64_t actual;
    __asm__ volatile("mov %%cr3, %0" : "=r"(actual));
    if (actual != (uint64_t)pml4) fail("own CR3", actual);
    serial("OWN_PAGE_TABLES_OK null_and_stack_guards=present\n");
}

#define PAGE_COUNT (PHYSICAL_LIMIT / 4096)
static uint64_t mapping_page_alloc(void);
/* Own identity mappings have no aliases. Only the boot CPU runs. Device
 * registers use PAT[3]=UC; framebuffer pixels alone may use PAT[4]=WC.
 * Split huge pages so changing framebuffer policy cannot affect an adjacent
 * register BAR. Never remove the null/stack guards when splitting a region. */
static void *physical_map(uint64_t address, size_t bytes, unsigned wc) {
    uint32_t a, b, c, d;
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(1), "c"(0));
    if (!(d & (1 << 16))) fail("CPU lacks PAT", d);
    __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000000U), "c"(0));
    unsigned bits = 36;
    if (a >= 0x80000008U) {
        __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(0x80000008U), "c"(0));
        bits = a & 255;
    }
    if (bits < 32 || bits > 52) fail("CPU physical-address width", bits);
    if (bits > 47) bits = 47;
    uint64_t limit = 1ULL << bits;
    if (!bytes || address < 0x100000 || address >= limit || bytes > limit - address)
        fail("device mapping bounds", address);
    uint32_t pat_low, pat_high;
    __asm__ volatile("rdmsr" : "=a"(pat_low), "=d"(pat_high) : "c"(0x277));
    if (pat_low >> 24) fail("PAT slot 3 must be uncached", pat_low);
    /* Conservative cache-type transition: interrupts off, no-fill mode,
     * write back/invalidate, invalidate all translations, program PAT/PTEs,
     * invalidate again, restore caching and interrupt state. MTRRs stay intact.
     * See Intel SDM vol.3, Memory Cache Control / PAT initialization. */
    uint64_t flags, cr0, cr3, cr4;
    __asm__ volatile("pushfq; pop %0; cli; mov %%cr0, %1; mov %%cr3, %2; mov %%cr4, %3"
                     : "=r"(flags), "=r"(cr0), "=r"(cr3), "=r"(cr4) : : "memory");
    uint64_t nofill = (cr0 | (1ULL << 30)) & ~(1ULL << 29), nonglobal = cr4 & ~(1ULL << 7);
    __asm__ volatile("mov %0, %%cr0; wbinvd; mov %1, %%cr4; mov %2, %%cr3"
                     : : "r"(nofill), "r"(nonglobal), "r"(cr3) : "memory");
    if (wc) {
        pat_high = (pat_high & ~255U) | 1; /* PAT4: write combining. */
        __asm__ volatile("wrmsr" : : "a"(pat_low), "d"(pat_high), "c"(0x277) : "memory");
    }
    uint64_t end = address + bytes;
    for (uint64_t at = address & ~4095ULL; at < end; at += 4096) {
        unsigned i = at >> 39 & 511, j = at >> 30 & 511, k = at >> 21 & 511;
        if (!pml4[i]) {
            uint64_t page = mapping_page_alloc();
            if (!page) fail("device PDPT allocation", at);
            pml4[i] = page | 3;
        }
        uint64_t *middle = (void *)(pml4[i] & ~4095ULL);
        if (!middle[j]) {
            uint64_t page = mapping_page_alloc();
            if (!page) fail("device PD allocation", at);
            middle[j] = page | 3;
        }
        uint64_t *directory = (void *)(middle[j] & ~4095ULL);
        if (!directory[k] || (directory[k] & 128)) {
            uint64_t page = mapping_page_alloc();
            if (!page) fail("device PT allocation", at);
            if (directory[k]) {
                uint64_t base = directory[k] & ~0x1fffffULL;
                if (base != (at & ~0x1fffffULL)) fail("nonidentity device PDE", at);
                uint64_t *entries = (void *)page;
                for (unsigned n = 0; n < 512; ++n) entries[n] = (base + n * 4096ULL) | 3;
            }
            directory[k] = page | 3;
        }
        uint64_t *entries = (void *)(directory[k] & ~4095ULL);
        unsigned n = at >> 12 & 511;
        if ((at >= (uint64_t)kernel_stack_guard && at < (uint64_t)kernel_stack_bottom) ||
            (at >= boot.kernel_start && at < boot.kernel_end))
            fail("device mapping overlaps kernel", at);
        entries[n] = at | (wc ? 0x83 : 0x1b); /* 4K PAT4 vs PAT3. */
    }
    __asm__ volatile("mov %0, %%cr3; wbinvd; mov %1, %%cr0; mov %2, %%cr4"
                     : : "r"(cr3), "r"(cr0), "r"(cr4) : "memory");
    if (flags & 0x200) __asm__ volatile("sti" : : : "memory");
    return (void *)address;
}
void *device_map(uint64_t address, size_t bytes) { return physical_map(address, bytes, 0); }
void *framebuffer_map(uint64_t address, size_t bytes) {
#ifdef BOOT_FRAMEBUFFER_UC
    serial("FRAMEBUFFER_CACHE requested=uc\n");
    return physical_map(address, bytes, 0);
#else
    serial("FRAMEBUFFER_CACHE requested=wc (MTRRs retained)\n");
    return physical_map(address, bytes, 1);
#endif
}

static uint8_t used[PAGE_COUNT / 8], allocated[PAGE_COUNT / 8];
static uint64_t free_pages, mapping_pages, next_page = 256, memory_limit_page = 256;
uint64_t page_available(void) { return free_pages; }
uint64_t page_mapping_owned(void) { return mapping_pages; }
static int bit(const uint8_t *map, uint64_t page) { return (map[page >> 3] >> (page & 7)) & 1; }
static void mark(uint8_t *map, uint64_t page, int value) {
    if (value) map[page >> 3] |= 1U << (page & 7);
    else map[page >> 3] &= ~(1U << (page & 7));
}
uint64_t page_alloc(void) {
    if (!free_pages) return 0;
    for (uint64_t scanned = 0; scanned < PAGE_COUNT; ++scanned) {
        uint64_t page = next_page++;
        if (next_page == PAGE_COUNT) next_page = 256;
        if (!bit(used, page)) {
            mark(used, page, 1); mark(allocated, page, 1); --free_pages;
            memset((void *)(page * 4096), 0, 4096);
            return page * 4096;
        }
    }
    fail("physical allocator accounting", free_pages);
}
/* These tables remain reachable from the kernel CR3 for its entire lifetime.
 * Transfer ownership out of the releasable allocation set; page_free must
 * reject them. Only physical_map may create this reservation. In particular,
 * late userland-triggered BAR mappings are not process-context allocations. */
static uint64_t mapping_page_alloc(void) {
    uint64_t address = page_alloc();
    if (address) {
        mark(allocated, address / 4096, 0);
        ++mapping_pages;
    }
    return address;
}
void page_free(uint64_t address) {
    if (!address || address % 4096 || address >= PHYSICAL_LIMIT || !bit(allocated, address / 4096))
        fail("invalid physical page free", address);
    uint64_t page = address / 4096;
    mark(allocated, page, 0); mark(used, page, 0); ++free_pages;
    if (page < next_page) next_page = page;
}
/* Contiguous kernel metadata allocations; all pages remain accounted for by
 * the same physical allocator. Process RAM itself uses individual COW pages. */
uint64_t pages_alloc(unsigned count) {
    if (!count || count > free_pages) return 0;
    unsigned run = 0;
    for (uint64_t page = 256; page < memory_limit_page; ++page) {
        if (bit(used, page)) { run = 0; continue; }
        if (++run != count) continue;
        uint64_t first = page + 1 - count;
        for (uint64_t p = first; p <= page; ++p) {
            mark(used, p, 1); mark(allocated, p, 1);
        }
        free_pages -= count;
        memset((void *)(first * 4096), 0, (size_t)count * 4096);
        return first * 4096;
    }
    return 0;
}
static void memory(void) {
    memset(used, 0xff, sizeof(used));
    if (boot.descriptor_size < sizeof(BootMemoryDescriptor) || !boot.memory_map_size ||
        boot.memory_map_size % boot.descriptor_size) fail("boot memory map", boot.descriptor_size);
    for (uint64_t at = 0; at < boot.memory_map_size; at += boot.descriptor_size) {
        const BootMemoryDescriptor *entry = (const void *)(boot.memory_map + at);
        if (entry->type != 7) continue;
        if (entry->physical_start % 4096 || entry->pages > (UINT64_MAX - entry->physical_start) / 4096)
            fail("physical memory descriptor", entry->physical_start);
        uint64_t start = entry->physical_start / 4096;
        uint64_t end = start + entry->pages;
        if (end > PAGE_COUNT) end = PAGE_COUNT;
        if (end > memory_limit_page) memory_limit_page = end;
        if (start < 256) start = 256;
        for (uint64_t page = start; page < end; ++page)
            if (bit(used, page)) { mark(used, page, 0); ++free_pages; }
    }
    uint64_t before = free_pages, a = page_alloc(), b = page_alloc();
    if (!a || !b || a == b || (a >= boot.kernel_start && a < boot.kernel_end) ||
        (b >= boot.kernel_start && b < boot.kernel_end)) fail("page allocator overlap", a);
    *(volatile uint64_t *)a = 0x123456789abcdef0ULL;
    *(volatile uint64_t *)b = 0xfedcba9876543210ULL;
    if (*(volatile uint64_t *)a != 0x123456789abcdef0ULL) fail("physical page alias", a);
    page_free(a);
    uint64_t reused = page_alloc();
    if (reused != a || *(volatile uint64_t *)reused) fail("page reuse and zeroing", reused);
    page_free(reused); page_free(b);
    uint64_t run = pages_alloc(3);
    if (!run) fail("contiguous page allocation", 3);
    for (unsigned i = 0; i < 3; ++i) {
        if (*(volatile uint64_t *)(run + i * 4096)) fail("contiguous page zeroing", run);
        page_free(run + i * 4096);
    }
    if (free_pages != before) fail("physical free count", free_pages);
    serial("PAGE_ALLOCATOR_OK free_pages="); hex(free_pages); serial("\n");
}


_Noreturn void exception_fatal(const uint64_t *frame) {
    uint64_t address;
    __asm__ volatile("mov %%cr2, %0" : "=r"(address));
    serial("EXCEPTION vector="); hex(frame[0]); serial(" error="); hex(frame[1]);
    serial(" rip="); hex(frame[2]); serial(" cr2="); hex(address); serial("\n");
    boot_diagnostic("CPU exception vector", frame[0]);
    boot_diagnostic("Instruction address", frame[2]);
    boot_diagnostic("Fault address (CR2)", address);
    stop(0x11);
}
_Noreturn void kernel_main(const BootInfo *information) {
    serial("ELF_KERNEL_ENTRY\n");
    if (information->magic != BOOT_INFO_MAGIC || information->size != sizeof(BootInfo))
        fail("boot information version", information->magic);
    boot = *information;
    uint64_t initial_cr0, initial_cr4;
    uint32_t efer_low, efer_high;
    __asm__ volatile("mov %%cr0, %0" : "=r"(initial_cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(initial_cr4));
    __asm__ volatile("rdmsr" : "=a"(efer_low), "=d"(efer_high) : "c"(0xc0000080));
    (void)efer_high;
    if (!(initial_cr0 & 1) || !(initial_cr0 & (1ULL << 31)) ||
        !(initial_cr4 & (1 << 5)) || !(efer_low & (1 << 10))) fail("long mode", initial_cr0);
    serial("LONG_MODE_OK\n");
    uint64_t rsp;
    __asm__ volatile("mov %%rsp, %0" : "=r"(rsp));
    if (rsp < (uint64_t)kernel_stack_bottom || rsp >= (uint64_t)kernel_stack_top)
        fail("own kernel stack", rsp);
    serial("OWN_STACK_OK\n");
    tables();
    paging();
    memory();
    /* RCC floating operations use baseline SSE2. IRQ entry saves x87/SSE;
     * scheduling still happens only after native activations return. */
    uint64_t cr0, cr4;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    cr0 = (cr0 & ~12ULL) | 2; cr4 |= 3ULL << 9;
    __asm__ volatile("mov %0, %%cr0; mov %1, %%cr4; fninit" : : "r"(cr0), "r"(cr4) : "memory");
    const uint32_t mxcsr = 0x1f80;
    __asm__ volatile("ldmxcsr %0" : : "m"(mxcsr));
#ifdef BOOT_HIGH_FRAMEBUFFER_TEST
    extern void high_framebuffer_prepare(void), high_framebuffer_check(void);
    BootInfo display = boot;
    high_framebuffer_prepare();
    console_init();
    high_framebuffer_check();
    boot = display;
#endif
    console_init();
#ifdef BOOT_TEST
    extern void console_check(void);
    console_check();
#endif
    acpi_init();
#ifdef BOOT_TOUCHPAD
    /* Size unresolved firmware BAR reservations once, with interrupts still
     * disabled and before any native device driver acquires its hardware. */
    (void)pci_resources_refine();
#endif
    timer_init();
#ifdef BOOT_NVME_PROBE
    extern void nvme_probe(void);
    nvme_probe();
#endif
#if BOOT_FAULT == 6
    __asm__ volatile("ud2");
#elif BOOT_FAULT == 14
    __asm__ volatile("movq $1, 0" : : : "memory");
#elif BOOT_FAULT == 8
    /* Keep exception delivery inside the absent page, so #PF escalates to
     * #DF and must use the independent IST stack. */
    __asm__ volatile("lea kernel_stack_guard+2048(%%rip), %%rsp; pushq $0" : : : "memory");
#endif
    runtime_run();
    console_flush();
    console_report();
    serial("RIPLIX_X86_64_BOOT_OK\n");
    stop(0x10);
}
