#ifndef RIPLIX_X86_BOOTINFO_H
#define RIPLIX_X86_BOOTINFO_H
#include <stddef.h>
#include <stdint.h>

#define BOOT_INFO_MAGIC 0x31544f4f42585052ULL
#define PHYSICAL_LIMIT (64ULL * 1024 * 1024 * 1024)
typedef struct {
    uint64_t magic, size;
    uint64_t kernel_start, kernel_end;
    uint64_t framebuffer, framebuffer_size;
    uint32_t width, height, stride, pixel_format;
    uint64_t memory_map, memory_map_size, descriptor_size;
    uint64_t tsc_hz, tsc_start, epoch;
    uint64_t rsdp;
} BootInfo;
typedef struct {
    uint32_t type, padding;
    uint64_t physical_start, virtual_start, pages, attributes;
} BootMemoryDescriptor;
#endif
