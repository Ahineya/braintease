#ifndef RIPLIX_X86_BOOTINFO_H
#define RIPLIX_X86_BOOTINFO_H
#include <stddef.h>
#include <stdint.h>

#define BOOT_INFO_MAGIC 0x31544f4f42585052ULL
#define PHYSICAL_LIMIT (64ULL * 1024 * 1024 * 1024)
/* Firmware PCI resource snapshot, copied before ExitBootServices. These are
 * host physical apertures/reservations, never payload pages or free-space
 * promises. Only translation-zero descriptors are accepted in version 1. */
#define PCI_BOOT_WINDOWS 16U
#define PCI_BOOT_SPANS 256U
#define PCI_BOOT_COMPLETE 1U
#define PCI_BOOT_INCOMPLETE 2U
#define PCI_BOOT_BAR 1U
#define PCI_BOOT_BRIDGE 2U
#define PCI_BOOT_ROM32 3U
#define PCI_BOOT_UNKNOWN 4U
typedef struct {
    uint64_t base, length;
    uint32_t segment;
    uint8_t bus_first, bus_last, prefetch, reserved;
} PciBootWindow;
typedef struct {
    uint64_t base, length;
    uint32_t segment;
    uint16_t bdf;
    uint8_t bar, kind;
} PciBootSpan;
typedef struct {
    uint32_t version, status, window_count, span_count;
    uint64_t target_bar0_base, target_bar0_length;
    char reason[96];
    PciBootWindow windows[PCI_BOOT_WINDOWS];
    PciBootSpan occupied[PCI_BOOT_SPANS];
} PciBootResources;
typedef struct {
    uint64_t magic, size;
    uint64_t kernel_start, kernel_end;
    uint64_t framebuffer, framebuffer_size;
    uint32_t width, height, stride, pixel_format;
    uint64_t memory_map, memory_map_size, descriptor_size;
    uint64_t tsc_hz, tsc_start, epoch;
    uint64_t rsdp;
    PciBootResources pci;
} BootInfo;
typedef struct {
    uint32_t type, padding;
    uint64_t physical_start, virtual_start, pages, attributes;
} BootMemoryDescriptor;
#endif
