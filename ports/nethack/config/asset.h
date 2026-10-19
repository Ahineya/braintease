#ifndef RIPPLE_NETHACK_ASSET_H
#define RIPPLE_NETHACK_ASSET_H
#include <stdio.h>
typedef struct nh_asset { FILE *file; int packed; } nh_asset;
nh_asset *nh_asset_open(const char *, const char *);
int nh_asset_close(nh_asset *);
size_t nh_asset_read(void *, size_t, size_t, nh_asset *);
int nh_asset_seek(nh_asset *, long, int);
char *nh_asset_gets(char *, int, nh_asset *);
int nh_asset_getc(nh_asset *);
long nh_asset_tell(nh_asset *);
#endif
