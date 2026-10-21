/* Minimal x64 UEFI interfaces used by the bring-up probe. Field order follows
 * UEFI 2.11 sections 4, 7 and 12. Unused service slots are intentionally opaque. */
#ifndef RIPLIX_BOOT_UEFI_H
#define RIPLIX_BOOT_UEFI_H
#include <stddef.h>
#include <stdint.h>

typedef uint64_t Status;
typedef void *Handle;
#define EFI_ERROR(n) (0x8000000000000000ULL | (n))
#define EFI_INVALID_PARAMETER EFI_ERROR(2)
#define EFI_BUFFER_TOO_SMALL EFI_ERROR(5)
#define EFI_LOADER_DATA 2

typedef struct {
    uint64_t signature;
    uint32_t revision, header_size, crc32, reserved;
} TableHeader;
typedef struct {
    uint32_t a;
    uint16_t b, c;
    uint8_t d[8];
} Guid;
typedef struct { Guid guid; void *table; } ConfigurationTable;
typedef struct TextOutput {
    void *reset;
    Status (*output_string)(struct TextOutput *, const uint16_t *);
} TextOutput;
typedef struct {
    uint32_t type, padding;
    uint64_t physical_start, virtual_start, pages, attributes;
} MemoryDescriptor;

typedef struct {
    TableHeader header;
    void *raise_tpl, *restore_tpl;
    Status (*allocate_pages)(uint32_t, uint32_t, size_t, uint64_t *);
    void *free_pages;
    Status (*get_memory_map)(size_t *, void *, size_t *, size_t *, uint32_t *);
    Status (*allocate_pool)(uint32_t, size_t, void **);
    void *free_pool, *create_event, *set_timer, *wait_for_event;
    void *signal_event, *close_event, *check_event;
    void *install_protocol_interface, *reinstall_protocol_interface;
    void *uninstall_protocol_interface, *handle_protocol, *reserved;
    void *register_protocol_notify, *locate_handle, *locate_device_path;
    void *install_configuration_table, *load_image, *start_image, *exit, *unload_image;
    Status (*exit_boot_services)(Handle, size_t);
    void *get_next_monotonic_count;
    Status (*stall)(size_t);
    Status (*set_watchdog_timer)(size_t, uint64_t, size_t, const uint16_t *);
    void *connect_controller, *disconnect_controller, *open_protocol, *close_protocol;
    void *open_protocol_information, *protocols_per_handle, *locate_handle_buffer;
    Status (*locate_protocol)(const Guid *, void *, void **);
} BootServices;

typedef struct {
    TableHeader header;
    uint16_t *firmware_vendor;
    uint32_t firmware_revision;
    Handle console_in_handle;
    void *console_in;
    Handle console_out_handle;
    TextOutput *console_out;
    Handle stderr_handle;
    void *stderr_output, *runtime_services;
    BootServices *boot_services;
    size_t configuration_entries;
    ConfigurationTable *configuration_table;
} SystemTable;

typedef struct {
    uint32_t version, width, height, pixel_format;
    uint32_t red_mask, green_mask, blue_mask, reserved_mask;
    uint32_t stride;
} GraphicsInfo;
typedef struct {
    uint32_t max_mode, mode;
    GraphicsInfo *info;
    size_t info_size;
    uint64_t framebuffer;
    size_t framebuffer_size;
} GraphicsMode;
typedef struct {
    void *query_mode, *set_mode, *blt;
    GraphicsMode *mode;
} Graphics;

_Static_assert(sizeof(void *) == 8, "UEFI probe requires x86_64 pointers");
_Static_assert(sizeof(TableHeader) == 24, "UEFI table header layout");
_Static_assert(sizeof(MemoryDescriptor) == 40, "UEFI memory descriptor layout");
_Static_assert(offsetof(SystemTable, boot_services) == 96, "UEFI system table layout");
_Static_assert(offsetof(BootServices, get_memory_map) == 56, "UEFI memory map slot");
_Static_assert(offsetof(BootServices, exit_boot_services) == 232, "UEFI exit slot");
_Static_assert(offsetof(BootServices, locate_protocol) == 320, "UEFI locate protocol slot");
_Static_assert(offsetof(GraphicsMode, framebuffer) == 24, "UEFI graphics layout");
#endif
