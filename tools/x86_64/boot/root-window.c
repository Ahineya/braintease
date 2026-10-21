#include "root-window.h"
static int equal(const uint8_t *a, const uint8_t *b, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static unsigned u16(const uint8_t *p) { return p[0] | (unsigned)p[1] << 8; }
static uint32_t u32(const uint8_t *p) { return u16(p) | (uint32_t)u16(p + 2) << 16; }
int root_window_contains(const RootWindow *w, uint64_t at, uint64_t bytes) {
    return w->ready && at >= w->offset && at - w->offset <= w->bytes &&
           bytes <= w->bytes - (at - w->offset);
}
int root_window_translate(const RootWindow *w, uint64_t at, size_t bytes, uint64_t *out) {
    if (!w->ready || at > w->bytes || bytes > w->bytes - at || at > UINT64_MAX - w->offset) return -1;
    *out = w->offset + at;
    return 0;
}
const char *root_window_open(GptRead read, void *ctx, uint64_t bytes, unsigned sector,
                             const RootIdentity *id, RootWindow *out) {
    /* Boot-time singleton. Keep the ~14KiB table outside the guarded stack. */
    static GptInfo table;
    static const uint8_t type[16] = RIPLIX_ROOT_TYPE;
    *out = (RootWindow){0};
    if (sector != id->sector) return "root partition sector identity mismatch";
    const char *error = gpt_discover(read, ctx, bytes, sector, &table);
    if (error) return error;
    if (!equal(id->disk, table.guid, 16)) return "root disk GUID mismatch";
    const GptPartition *p = 0;
    for (unsigned i = 0; i < table.count; ++i)
        if (equal(table.partitions[i].guid, id->partition, 16)) p = &table.partitions[i];
    if (!p) return "root partition GUID missing";
    if (!equal(p->type, type, 16)) return "root partition type mismatch";
    if (p->attributes) return "root partition attributes unsupported";
    if (p->first != id->first || p->last != id->last) return "root partition geometry mismatch";
    uint64_t offset = p->first * sector, length = (p->last - p->first + 1) * sector;
    if (offset % 2048 || length < 16384 || length % 2048)
        return "root partition alignment/size";
    uint8_t header[4096];
    if (read(ctx, offset, header, sizeof(header))) return "root partition header read failed";
    if (!equal(header, (const uint8_t *)"RIPPLE-FUZIX-C16-V3", 19) || u16(header + 2048) != 0x7254)
        return "root partition C16 V3 signature";
    uint64_t blocks = u32(header + 2052), inodes_end = u16(header + 2050);
    if (inodes_end < 3 || blocks <= inodes_end + 8 || blocks * 2048 > length || blocks * 2048 > 0x7fffffff ||
        blocks * 2048 < 16384 || blocks * 2048 % sector)
        return "root partition filesystem bounds";
    *out = (RootWindow){.offset=offset, .bytes=blocks * 2048, .ready=1};
    return 0;
}
