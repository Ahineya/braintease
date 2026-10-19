/* Portable container for guest-generated C16 data on octet-only host shares.
 * Game saves still use NetHack's normal guest filesystem representation. */
#include "asset.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#define MAGIC "NHC16v1\n"
#define HEADER 8

nh_asset *nh_asset_open(const char *name, const char *mode)
{
    nh_asset *asset;
    char magic[HEADER];
    FILE *file = fopen(name,mode);
    if (!file) return NULL;
    asset = malloc(sizeof *asset);
    if (!asset) { fclose(file); return NULL; }
    asset->file = file;
    asset->packed = fread(magic,1,HEADER,file) == HEADER && !memcmp(magic,MAGIC,HEADER);
    if (!asset->packed) rewind(file);
    return asset;
}
int nh_asset_close(nh_asset *asset)
{
    int result = fclose(asset->file);
    free(asset);
    return result;
}
size_t nh_asset_read(void *buffer, size_t size, size_t count, nh_asset *asset)
{
    size_t i, total;
    unsigned char *out = buffer;
    if (!asset->packed) return fread(buffer,size,count,asset->file);
    if (!size || !count) return 0;
    if (count > ULONG_MAX / size) { errno = EOVERFLOW; return 0; }
    total = size * count;
    for (i = 0; i < total; ++i) {
        int lo = fgetc(asset->file), hi;
        if (lo == EOF) break;
        hi = fgetc(asset->file);
        if (hi == EOF || lo > 255 || hi > 255) { errno = EIO; break; }
        out[i] = (unsigned)lo | ((unsigned)hi << 8);
    }
    return i / size;
}
int nh_asset_seek(nh_asset *asset, long offset, int whence)
{
    if (asset->packed) {
        if (offset > (LONG_MAX - HEADER) / 2 || offset < LONG_MIN / 2) { errno = EOVERFLOW; return -1; }
        offset *= 2;
        if (whence == SEEK_SET) offset += HEADER;
    }
    return fseek(asset->file,offset,whence);
}
long nh_asset_tell(nh_asset *asset)
{
    long pos = ftell(asset->file);
    return pos < 0 || !asset->packed ? pos : (pos - HEADER) / 2;
}
int nh_asset_getc(nh_asset *asset)
{
    unsigned char cell;
    if (!asset->packed) return fgetc(asset->file);
    return nh_asset_read(&cell,1,1,asset) == 1 ? (int)cell : EOF;
}
char *nh_asset_gets(char *out, int length, nh_asset *asset)
{
    int i = 0, ch;
    if (!asset->packed) return fgets(out,length,asset->file);
    if (length <= 0) return NULL;
    while (i + 1 < length && (ch = nh_asset_getc(asset)) != EOF) {
        out[i++] = ch;
        if (ch == '\n') break;
    }
    out[i] = 0;
    return i ? out : NULL;
}
