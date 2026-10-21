/* UEFI 2.11 chapter 5. Native byte/LBA arithmetic, independent of C16 FILE.
 * Bounded streaming parser: 128 entries, 128..1024-byte entry records, <12KiB
 * stack. Unknown partition types are reported without interpreting contents. */
#include "gpt.h"
static uint32_t u32(const uint8_t *p) {
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t u64(const uint8_t *p) { return u32(p) | (uint64_t)u32(p + 4) << 32; }
static int equal(const uint8_t *a, const uint8_t *b, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (a[i] != b[i]) return 0;
    return 1;
}
static int nonzero(const uint8_t *p, unsigned n) {
    for (unsigned i = 0; i < n; ++i) if (p[i]) return 1;
    return 0;
}
static uint32_t crc(uint32_t value, const uint8_t *p, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        value ^= p[i];
        for (unsigned bit = 0; bit < 8; ++bit) value = value >> 1 ^ (0xedb88320U & (0U - (value & 1)));
    }
    return value;
}
typedef struct {
    uint64_t current, alternate, first, last, array;
    uint32_t count, entry_size, array_crc;
    uint8_t guid[16];
} Header;
static const char *header(GptRead read, void *ctx, uint64_t sectors, unsigned sector,
                          uint64_t lba, Header *out) {
    uint8_t bytes[4096];
    if (read(ctx, lba * sector, bytes, sector)) return "GPT header read failed";
    if (!equal(bytes, (const uint8_t *)"EFI PART", 8) || u32(bytes + 8) != 0x10000)
        return "GPT signature/revision";
    unsigned length = u32(bytes + 12);
    if (length < 92 || length > sector || u32(bytes + 20)) return "GPT header size/reserved";
    uint32_t stored = u32(bytes + 16);
    for (unsigned i = 16; i < 20; ++i) bytes[i] = 0;
    if (~crc(UINT32_MAX, bytes, length) != stored) return "GPT header CRC";
    *out = (Header){.current=u64(bytes + 24), .alternate=u64(bytes + 32),
                    .first=u64(bytes + 40), .last=u64(bytes + 48), .array=u64(bytes + 72),
                    .count=u32(bytes + 80), .entry_size=u32(bytes + 84), .array_crc=u32(bytes + 88)};
    for (unsigned i = 0; i < 16; ++i) out->guid[i] = bytes[56 + i];
    if (out->current != lba || out->alternate != (lba == 1 ? sectors - 1 : 1) ||
        out->first > out->last || out->first < 3 || out->last >= sectors - 1 || !nonzero(out->guid, 16))
        return "GPT header LBA/GUID bounds";
    if (!out->count || out->count > GPT_MAX_PARTITIONS || out->entry_size < 128 || out->entry_size > 1024 ||
        (out->entry_size & (out->entry_size - 1))) return "GPT unsupported entry count/size";
    uint64_t size = (uint64_t)out->count * out->entry_size;
    uint64_t blocks = (size + sector - 1) / sector;
    if (size < 16384 || out->array >= sectors || blocks > sectors - out->array)
        return "GPT entry array bounds";
    if (lba == 1 ? out->array < 2 || out->array >= out->first || blocks > out->first - out->array :
                   out->array <= out->last || out->array >= lba || blocks > lba - out->array)
        return "GPT entry array overlaps usable/header LBAs";
    uint64_t offset = out->array * sector;
    uint32_t value = UINT32_MAX;
    for (uint64_t done = 0; done < size;) {
        unsigned n = size - done < sizeof(bytes) ? (unsigned)(size - done) : sizeof(bytes);
        if (read(ctx, offset + done, bytes, n)) return "GPT entry array read failed";
        value = crc(value, bytes, n); done += n;
    }
    return ~value == out->array_crc ? 0 : "GPT entry array CRC";
}
const char *gpt_discover(GptRead read, void *ctx, uint64_t bytes, unsigned sector, GptInfo *out) {
    *out = (GptInfo){0};
    if (sector < 512 || sector > 4096 || (sector & (sector - 1)) || bytes % sector || bytes / sector < 68)
        return "GPT device geometry";
    uint8_t mbr[512];
    if (read(ctx, 0, mbr, sizeof(mbr))) return "GPT protective MBR read failed";
    if (mbr[510] != 0x55 || mbr[511] != 0xaa) return "GPT protective MBR signature";
    uint64_t sectors = bytes / sector;
    unsigned protective = 0;
    for (unsigned i = 0; i < 4; ++i) {
        const uint8_t *entry = mbr + 446 + i * 16;
        if (!nonzero(entry, 16)) continue;
        if (entry[0] || entry[4] != 0xee || u32(entry + 8) != 1 ||
            u32(entry + 12) != (sectors - 1 > UINT32_MAX ? UINT32_MAX : sectors - 1))
            return "GPT hybrid/invalid protective MBR";
        ++protective;
    }
    if (protective != 1) return "GPT protective MBR entry count";
    Header primary, backup;
    const char *error = header(read, ctx, sectors, sector, 1, &primary);
    if (error) return error;
    error = header(read, ctx, sectors, sector, sectors - 1, &backup);
    if (error) return error;
    if (primary.first != backup.first || primary.last != backup.last || primary.count != backup.count ||
        primary.entry_size != backup.entry_size || primary.array_crc != backup.array_crc ||
        !equal(primary.guid, backup.guid, 16)) return "GPT primary/backup disagreement";
    /* Equal CRCs are not a substitute for checking that the arrays agree. */
    uint8_t entry[1024], other[1024];
    for (unsigned i = 0; i < primary.count; ++i) {
        uint64_t at = (uint64_t)i * primary.entry_size;
        if (read(ctx, primary.array * sector + at, entry, primary.entry_size) ||
            read(ctx, backup.array * sector + at, other, primary.entry_size)) return "GPT partition entry read failed";
        if (!equal(entry, other, primary.entry_size)) return "GPT entry arrays disagree";
        if (!nonzero(entry, 16)) continue;
        uint64_t first = u64(entry + 32), last = u64(entry + 40);
        if (!nonzero(entry + 16, 16) || first < primary.first || first > last || last > primary.last)
            return "GPT partition bounds/GUID";
        for (unsigned j = 0; j < out->count; ++j) {
            const GptPartition *p = &out->partitions[j];
            if (equal(entry + 16, p->guid, 16)) return "GPT duplicate partition GUID";
            if (first <= p->last && p->first <= last) return "GPT overlapping partitions";
        }
        GptPartition *p = &out->partitions[out->count++];
        p->index = i + 1; p->first = first; p->last = last; p->attributes = u64(entry + 48);
        for (unsigned n = 0; n < 16; ++n) { p->type[n] = entry[n]; p->guid[n] = entry[16 + n]; }
        for (unsigned n = 0; n < 36; ++n) {
            unsigned c = entry[56 + n * 2] | (unsigned)entry[57 + n * 2] << 8;
            p->name[n] = c >= 32 && c <= 126 ? (char)c : c ? '?' : 0;
        }
    }
    for (unsigned i = 0; i < 16; ++i) out->guid[i] = primary.guid[i];
    out->first_usable = primary.first; out->last_usable = primary.last; out->sector_size = sector;
    return 0;
}
