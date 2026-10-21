/* Disposable FAT16 boot media, streamed with bounded memory. No device I/O. */
#include "link.h"
#include <stdlib.h>
#include <string.h>
#define MEDIA_BYTES (16UL * 1024 * 1024)
#define FAT_SECTORS 128UL
#define ROOT_SECTOR 257UL
#define DATA_SECTOR 289UL
#define PAYLOAD_START ((DATA_SECTOR + 2) * 512UL)

static void entry(unsigned char *p, const char *name, unsigned attributes, L32 cluster, L32 size)
{
    memcpy(p, name, 11);
    p[11] = attributes;
    ld_patch(p + 22, 0, 2);
    ld_patch(p + 24, 33, 2);
    ld_patch(p + 26, cluster, 2);
    ld_patch(p + 28, size, 4);
}

void ld_boot_media(const char *path, FILE *out)
{
    FILE *in;
    long length = 0;
    L32 size, count, i, at;
    unsigned char *prefix, *fat, buffer[4096];
    ld_path = path;
    in = fopen(path, "rb");
    if (!in || fseek(in, 0, SEEK_END) || (length = ftell(in)) < 64 ||
        (unsigned long)length > MEDIA_BYTES - PAYLOAD_START || fseek(in, 0, SEEK_SET))
        ld_error("cannot read bounded EFI boot payload");
    if (fread(buffer, 1, 64, in) != 64 || buffer[0] != 'M' || buffer[1] != 'Z' ||
        fseek(in, 0, SEEK_SET))
        ld_error("boot payload must be an EFI executable");
    size = (L32)length;
    count = (size + 511) / 512;
    prefix = ld_memory(PAYLOAD_START);
    fat = prefix + 512;
    memcpy(prefix, "\353\074\220RIPLIX  ", 11);
    ld_patch(prefix + 11, 512, 2);
    prefix[13] = 1;
    ld_patch(prefix + 14, 1, 2);
    prefix[16] = 2;
    ld_patch(prefix + 17, 512, 2);
    ld_patch(prefix + 19, MEDIA_BYTES / 512, 2);
    prefix[21] = 0xf8;
    ld_patch(prefix + 22, FAT_SECTORS, 2);
    ld_patch(prefix + 24, 32, 2);
    ld_patch(prefix + 26, 64, 2);
    prefix[36] = 0x80;
    prefix[38] = 0x29;
    ld_patch(prefix + 39, 0x5249504cUL, 4);
    memcpy(prefix + 43, "RIPLIXBOOT FAT16   ", 19);
    prefix[510] = 0x55;
    prefix[511] = 0xaa;
    for (i = 0; i < 4; i++)
        ld_patch(fat + i * 2, i ? 65535 : 65528, 2);
    for (i = 4; i < 4 + count; i++)
        ld_patch(fat + i * 2, i < 3 + count ? i + 1 : 65535, 2);
    memcpy(prefix + (1 + FAT_SECTORS) * 512, fat, FAT_SECTORS * 512);
    entry(prefix + ROOT_SECTOR * 512, "EFI        ", 0x10, 2, 0);
    entry(prefix + DATA_SECTOR * 512, ".          ", 0x10, 2, 0);
    entry(prefix + DATA_SECTOR * 512 + 32, "..         ", 0x10, 0, 0);
    entry(prefix + DATA_SECTOR * 512 + 64, "BOOT       ", 0x10, 3, 0);
    entry(prefix + (DATA_SECTOR + 1) * 512, ".          ", 0x10, 3, 0);
    entry(prefix + (DATA_SECTOR + 1) * 512 + 32, "..         ", 0x10, 2, 0);
    entry(prefix + (DATA_SECTOR + 1) * 512 + 64, "BOOTX64 EFI", 0x20, 4, size);
    if (fwrite(prefix, 1, PAYLOAD_START, out) != PAYLOAD_START)
        ld_error("cannot write FAT16 metadata");
    free(prefix);
    for (at = 0; at < size;) {
        L32 n = size - at < sizeof buffer ? size - at : sizeof buffer;
        if (fread(buffer, 1, n, in) != n)
            ld_error("cannot read EFI payload");
        for (i = 0; i < n; i++)
            if (buffer[i] > 255)
                ld_error("EFI payload contains non-octets");
        if (fwrite(buffer, 1, n, out) != n)
            ld_error("cannot write EFI payload");
        at += n;
    }
    if (getc(in) != EOF || fclose(in))
        ld_error("EFI payload changed during media construction");
    memset(buffer, 0, sizeof buffer);
    for (at = PAYLOAD_START + size; at < MEDIA_BYTES;) {
        L32 n = MEDIA_BYTES - at < sizeof buffer ? MEDIA_BYTES - at : sizeof buffer;
        if (fwrite(buffer, 1, n, out) != n)
            ld_error("cannot finish FAT16 boot media");
        at += n;
    }
}
