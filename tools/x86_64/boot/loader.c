/* UEFI owns loading only. The embedded ELF kernel has its own stack, tables,
 * allocator and runtime, and makes no firmware calls after handoff. */
#include "uefi.h"
#include "bootinfo.h"
#include "io.h"
#ifdef BOOT_TOUCHPAD
#include "pci-uefi.h"
#endif

extern const uint8_t kernel_payload_start[], kernel_payload_end[];
static BootInfo boot;
/* Keep a real PE base relocation even when all emitted instructions happen
 * to be RIP-relative. Exercise firmware relocation of native data explicitly. */
static BootInfo *volatile boot_anchor = &boot;
static TextOutput *diagnostic_console;
void boot_diagnostic(const char *message, uint64_t status) {
    if (!diagnostic_console) return;
    uint16_t line[160];
    unsigned at = 0;
    const char *prefix = "Riplix boot failed: ";
    while (*prefix) line[at++] = (uint8_t)*prefix++;
    while (*message && at < 120) line[at++] = (uint8_t)*message++;
    line[at++] = ' '; line[at++] = '0'; line[at++] = 'x';
    for (int shift = 60; shift >= 0; shift -= 4) line[at++] = "0123456789abcdef"[status >> shift & 15];
    line[at++] = 13; line[at++] = 10; line[at] = 0;
    diagnostic_console->output_string(diagnostic_console, line);
}
typedef struct {
    uint8_t ident[16];
    uint16_t type, machine;
    uint32_t version;
    uint64_t entry, phoff, shoff;
    uint32_t flags;
    uint16_t ehsize, phentsize, phnum, shentsize, shnum, shstrndx;
} ElfHeader;
typedef struct {
    uint32_t type, flags;
    uint64_t offset, vaddr, paddr, filesz, memsz, align;
} ProgramHeader;
_Static_assert(sizeof(ElfHeader) == 64 && sizeof(ProgramHeader) == 56, "ELF64 layout");

static uint64_t load_kernel(BootServices *services) {
    size_t length = (size_t)(kernel_payload_end - kernel_payload_start);
    if (length < sizeof(ElfHeader)) fail("ELF header missing", length);
    const ElfHeader *header = (const void *)kernel_payload_start;
    if (header->ident[0] != 0x7f || header->ident[1] != 'E' || header->ident[2] != 'L' ||
        header->ident[3] != 'F' || header->ident[4] != 2 || header->ident[5] != 1 ||
        header->type != 3 || header->machine != 62 || header->version != 1 ||
        header->ehsize != sizeof(ElfHeader) || header->phentsize != sizeof(ProgramHeader) ||
        !header->phnum || header->phoff > length ||
        header->phnum > (length - header->phoff) / sizeof(ProgramHeader))
        fail("unsupported ELF kernel", 0);
    const ProgramHeader *segments = (const void *)(kernel_payload_start + header->phoff);
    uint64_t start = UINT64_MAX, end = 0;
    int entry_mapped = 0;
    for (unsigned i = 0; i < header->phnum; ++i) {
        const ProgramHeader *s = &segments[i];
        if (s->type == 3) fail("ELF interpreter is unsupported", s->type);
        if (s->type != 1 || !s->memsz) continue;
        if (s->paddr != s->vaddr || s->vaddr >= PHYSICAL_LIMIT ||
            s->memsz > PHYSICAL_LIMIT - s->vaddr || s->filesz > s->memsz ||
            s->offset > length || s->filesz > length - s->offset ||
            s->align < 4096 || s->align & (s->align - 1) ||
            s->vaddr % s->align != s->offset % s->align)
            fail("invalid ELF load segment", i);
        for (unsigned j = 0; j < i; ++j) {
            const ProgramHeader *p = &segments[j];
            if (p->type == 1 && p->memsz && s->vaddr < p->vaddr + p->memsz &&
                p->vaddr < s->vaddr + s->memsz) fail("overlapping ELF segments", i);
        }
        if (s->vaddr < start) start = s->vaddr;
        if (s->vaddr + s->memsz > end) end = s->vaddr + s->memsz;
        if ((s->flags & 1) && header->entry >= s->vaddr && header->entry < s->vaddr + s->filesz)
            entry_mapped = 1;
    }
    if (!entry_mapped || start != 0 || end <= start || end - start > 128 * 1024 * 1024)
        fail("ELF load range or entry", header->entry);
    end = (end + 4095) & ~4095ULL;
    uint64_t address = PHYSICAL_LIMIT - 1;
    Status status = services->allocate_pages(1, 1, (size_t)(end / 4096), &address);
    if (status || !address || address >= PHYSICAL_LIMIT || end > PHYSICAL_LIMIT - address)
        fail("reserve ELF kernel pages", status);
    for (uint64_t at = 0; at < end; ++at) ((volatile uint8_t *)address)[at] = 0;
    for (unsigned i = 0; i < header->phnum; ++i) {
        const ProgramHeader *s = &segments[i];
        if (s->type != 1) continue;
        for (uint64_t at = 0; at < s->filesz; ++at)
            ((volatile uint8_t *)(address + s->vaddr))[at] = kernel_payload_start[s->offset + at];
    }
    /* A statically linked PIE needs only R_X86_64_RELATIVE relocations.
     * No symbol lookup, host loader, dependencies or runtime resolver exists. */
    uint64_t rela = 0, rela_size = 0, rela_entry = 0;
    int dynamic = 0;
    for (unsigned i = 0; i < header->phnum; ++i) {
        const ProgramHeader *s = &segments[i];
        if (s->type != 2) continue;
        if (dynamic++ || s->vaddr >= end || s->memsz > end - s->vaddr || s->memsz % 16)
            fail("ELF dynamic table", i);
        const uint64_t *table = (const void *)(address + s->vaddr);
        int terminated = 0;
        for (uint64_t entry = 0; entry < s->memsz / 16; ++entry) {
            uint64_t tag = table[entry * 2], value = table[entry * 2 + 1];
            if (!tag) { terminated = 1; break; }
            if (tag == 1 || tag == 17 || tag == 23) fail("unsupported ELF dynamic entry", tag);
            if (tag == 7) rela = value;
            if (tag == 8) rela_size = value;
            if (tag == 9) rela_entry = value;
        }
        if (!terminated) fail("unterminated ELF dynamic table", 0);
    }
    if (!dynamic || rela_entry != 24 || rela_size % 24 || rela >= end || rela_size > end - rela)
        fail("ELF relocation table", rela);
    for (uint64_t at = 0; at < rela_size; at += 24) {
        const uint64_t *r = (const void *)(address + rela + at);
        if (r[1] != 8 || r[0] > end - 8 || r[2] >= end) fail("ELF relative relocation", r[1]);
        *(uint64_t *)(address + r[0]) = address + r[2];
    }
    boot.kernel_start = address; boot.kernel_end = address + end;
    serial("ELF_LOADED entry="); hex(address + header->entry); serial("\n");
    return address + header->entry;
}

static uint64_t clock_epoch(SystemTable *system) {
    struct Time {
        uint16_t year;
        uint8_t month, day, hour, minute, second, pad;
        uint32_t nanosecond;
        int16_t timezone;
        uint8_t daylight, pad2;
    } time;
    struct Runtime {
        TableHeader header;
        Status (*get_time)(struct Time *, void *);
    } *runtime = system->runtime_services;
    Status status = runtime->get_time(&time, 0);
    if (status || time.year < 1970 || time.year > 9999 || !time.month || time.month > 12 ||
        !time.day || time.day > 31 || time.hour > 23 || time.minute > 59 || time.second > 59)
        fail("firmware RTC snapshot", status);
    const uint8_t days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    uint64_t count = 0;
    for (unsigned year = 1970; year < time.year; ++year)
        count += 365 + (year % 4 == 0 && (year % 100 != 0 || year % 400 == 0));
    for (unsigned month = 1; month < time.month; ++month)
        count += days[month - 1] + (month == 2 && time.year % 4 == 0 &&
                                  (time.year % 100 != 0 || time.year % 400 == 0));
    uint64_t epoch = (count + time.day - 1) * 86400 + time.hour * 3600 + time.minute * 60 + time.second;
    if (time.timezone >= -1440 && time.timezone <= 1440)
        epoch = (uint64_t)((int64_t)epoch - (int64_t)time.timezone * 60);
    return epoch;
}

Status efi_main(Handle image, SystemTable *system) {
    serial_init();
    serial("Riplix x86_64 UEFI loader\n");
    if (boot_anchor != &boot) fail("UEFI loader data relocation", (uint64_t)boot_anchor);
    serial("UEFI_RELOCATION_OK\n");
    if (system->header.signature != 0x5453595320494249ULL)
        fail("UEFI system table signature", 0);
    diagnostic_console = system->console_out;
    BootServices *services = system->boot_services;
    const uint16_t banner[] = {'R','i','p','l','i','x',' ','x','8','6','_','6','4',':',' ',
        'l','o','a','d','i','n','g',' ','R','A','M',' ','k','e','r','n','e','l','.', '.', '.',13,10,0};
    if (system->console_out) system->console_out->output_string(system->console_out, banner);
    Status status = services->set_watchdog_timer(0, 0, 0, 0);
    if (status) fail("disable firmware watchdog", status);
    boot.magic = BOOT_INFO_MAGIC; boot.size = sizeof(boot);
    const Guid acpi_guid = {0x8868e871, 0xe4f1, 0x11d3,
                           {0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81}};
    if (system->configuration_entries > 4096) fail("UEFI configuration count", system->configuration_entries);
    for (size_t i = 0; i < system->configuration_entries; ++i) {
        const ConfigurationTable *t = &system->configuration_table[i];
        const uint8_t *a = (const void *)&t->guid, *b = (const void *)&acpi_guid;
        unsigned same = 1;
        for (unsigned j = 0; j < sizeof(Guid); ++j) if (a[j] != b[j]) same = 0;
        if (same) {
            if (boot.rsdp) fail("duplicate ACPI configuration", i);
            boot.rsdp = (uint64_t)t->table;
        }
    }
    if (!boot.rsdp) fail("UEFI ACPI 2 RSDP missing", 0);
    serial("RSDP_HANDOFF "); hex(boot.rsdp); serial("\n");
    uint64_t entry = load_kernel(services);
    const Guid graphics_guid = {0x9042a9de, 0x23dc, 0x4a38,
                               {0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a}};
    Graphics *graphics = 0;
    status = services->locate_protocol(&graphics_guid, 0, (void **)&graphics);
    if (status || !graphics || !graphics->mode || !graphics->mode->info)
        fail("graphics protocol", status);
    boot.framebuffer = graphics->mode->framebuffer;
    boot.framebuffer_size = graphics->mode->framebuffer_size;
    boot.width = graphics->mode->info->width; boot.height = graphics->mode->info->height;
    boot.stride = graphics->mode->info->stride; boot.pixel_format = graphics->mode->info->pixel_format;
    serial("GOP base="); hex(boot.framebuffer); serial(" size="); hex(boot.framebuffer_size);
    serial(" width="); hex(boot.width); serial(" height="); hex(boot.height);
    serial(" stride="); hex(boot.stride); serial(" format="); hex(boot.pixel_format); serial("\n");
    uint64_t before = rdtsc();
    status = services->stall(10000);
    boot.tsc_hz = (rdtsc() - before) * 100;
    if (status || boot.tsc_hz < 1000000 || boot.tsc_hz > 1000000000000ULL)
        fail("TSC calibration", boot.tsc_hz);
    boot.epoch = clock_epoch(system);
    boot.tsc_start = rdtsc();
#ifdef BOOT_TOUCHPAD
    pci_uefi_capture(services,&boot);
#endif

    size_t map_size = 0, key = 0, descriptor_size = 0;
    uint32_t version = 0;
    status = services->get_memory_map(&map_size, 0, &key, &descriptor_size, &version);
    if (status != EFI_BUFFER_TOO_SMALL || descriptor_size < sizeof(MemoryDescriptor) ||
        descriptor_size > 4096 || map_size > SIZE_MAX - descriptor_size * 32)
        fail("memory map sizing", status);
    size_t capacity = map_size + descriptor_size * 32;
    void *map = 0;
    status = services->allocate_pool(EFI_LOADER_DATA, capacity, &map);
    if (status) fail("memory map allocation", status);
    boot.memory_map = (uint64_t)map;
    for (unsigned attempt = 0; attempt < 4; ++attempt) {
        map_size = capacity;
        status = services->get_memory_map(&map_size, map, &key, &descriptor_size, &version);
        if (status || descriptor_size < sizeof(MemoryDescriptor) || map_size % descriptor_size)
            fail("final memory map", status);
        boot.memory_map_size = map_size; boot.descriptor_size = descriptor_size;
        /* Even a failed first exit attempt may partially shut services down.
         * Retry only memory-map/exit operations; no text protocol afterward. */
        diagnostic_console = 0;
        status = services->exit_boot_services(image, key);
        if (!status) {
            __asm__ volatile("cli; cld" : : : "memory");
            serial("EXIT_BOOT_SERVICES_OK\n");
            typedef void (__attribute__((sysv_abi)) *KernelEntry)(const BootInfo *);
            ((KernelEntry)entry)(&boot);
            fail("kernel returned", 0);
        }
        if (status != EFI_INVALID_PARAMETER) fail("ExitBootServices", status);
    }
    fail("memory map changed repeatedly", status);
}
