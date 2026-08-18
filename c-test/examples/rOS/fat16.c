#ifndef ROS_FAT16_C
#define ROS_FAT16_C

#include "fat16.h"
#include "disk.h"
#include <ctype.h>

static int is_sep(int c) {
    return c == '/' || c == '\\';
}

void fat16_canon83(const char *in, char out11[11]) {
    int i;
    int n;
    int e;

    i = 0;
    while (i < 11) {
        out11[i] = ' ';
        i = i + 1;
    }

    if (in[0] == '.' && in[1] == 0) {
        out11[0] = '.';
        return;
    }
    if (in[0] == '.' && in[1] == '.' && in[2] == 0) {
        out11[0] = '.';
        out11[1] = '.';
        return;
    }

    n = 0;
    e = 0;
    i = 0;
    while (in[i]) {
        if (in[i] == '.') {
            e = 1;
            n = 0;
            i = i + 1;
            continue;
        }
        if (e) {
            if (n < 3) {
                out11[8 + n] = (char)toupper((int)(unsigned char)in[i]);
                n = n + 1;
            }
        } else {
            if (n < 8) {
                out11[n] = (char)toupper((int)(unsigned char)in[i]);
                n = n + 1;
            }
        }
        i = i + 1;
    }
}

void fat16_format83(const uint8_t raw11[11], char out13[13]) {
    int i;
    int o;
    uint8_t c;

    o = 0;
    i = 0;
    while (i < 8) {
        c = raw11[i];
        if (i == 0 && c == 0x05) {
            c = 0xE5;
        }
        if (c != ' ') {
            out13[o] = (char)c;
            o = o + 1;
        }
        i = i + 1;
    }
    if (raw11[8] != ' ' || raw11[9] != ' ' || raw11[10] != ' ') {
        out13[o] = '.';
        o = o + 1;
        i = 8;
        while (i < 11) {
            if (raw11[i] != ' ') {
                out13[o] = (char)raw11[i];
                o = o + 1;
            }
            i = i + 1;
        }
    }
    out13[o] = 0;
}

int fat16_name_eq(const uint8_t raw11[11], const char *user) {
    char canon[11];
    int i;

    fat16_canon83(user, canon);
    i = 0;
    while (i < 11) {
        if ((char)raw11[i] != canon[i]) {
            return 0;
        }
        i = i + 1;
    }
    return 1;
}

int fat16_is_eof(uint16_t cluster) {
    return cluster >= 0xFFF8;
}

uint16_t fat16_fat_next(Fat16Fs *fs, uint16_t cluster) {
    uint32_t off;

    off = fs->fat_offset + (uint32_t)cluster * 2;
    return disk_read16(off);
}

uint32_t fat16_cluster_off(Fat16Fs *fs, uint16_t cluster) {
    uint32_t n;

    n = (uint32_t)(cluster - 2);
    return fs->data_offset + n * fs->cluster_bytes;
}

int fat16_mount(Fat16Fs *fs) {
    uint16_t bps;
    uint16_t spc;
    uint16_t reserved;
    uint16_t fats;
    uint16_t root;
    uint16_t tot16;
    uint16_t spf;
    uint32_t tot32;
    uint16_t sig;
    uint16_t root_secs;
    uint32_t fat_off;
    uint32_t root_off;
    uint32_t data_off;
    int i;
    uint8_t c;

    bps = disk_read16(11);
    spc = disk_read8(13);
    reserved = disk_read16(14);
    fats = disk_read8(16);
    root = disk_read16(17);
    tot16 = disk_read16(19);
    spf = disk_read16(22);
    tot32 = disk_read32(32);
    sig = disk_read16(510);

    if (sig != 0xAA55) {
        return FAT16_ERR_NOTFAT;
    }
    if (bps != 512 && bps != 1024 && bps != 2048 && bps != 4096) {
        return FAT16_ERR_NOTFAT;
    }
    if (spc == 0 || fats == 0) {
        return FAT16_ERR_NOTFAT;
    }

    if (tot16 != 0) {
        tot32 = tot16;
    }

    root_secs = (uint16_t)(((uint32_t)root * 32 + (uint32_t)(bps - 1)) / bps);
    fat_off = (uint32_t)reserved * bps;
    root_off = fat_off + (uint32_t)fats * (uint32_t)spf * bps;
    data_off = root_off + (uint32_t)root_secs * bps;

    fs->bytes_per_sec = bps;
    fs->sec_per_clus = spc;
    fs->reserved_sec = reserved;
    fs->num_fats = fats;
    fs->root_entries = root;
    fs->secs_per_fat = spf;
    fs->root_secs = root_secs;
    fs->total_secs = tot32;
    fs->fat_offset = fat_off;
    fs->root_offset = root_off;
    fs->data_offset = data_off;
    fs->cluster_bytes = (uint32_t)spc * bps;
    fs->vol_id = disk_read32(39);

    i = 0;
    while (i < 11) {
        c = disk_read8(43 + i);
        fs->label[i] = (char)c;
        i = i + 1;
    }
    fs->label[11] = 0;
    i = 10;
    while (i >= 0 && fs->label[i] == ' ') {
        fs->label[i] = 0;
        i = i - 1;
    }

    return FAT16_OK;
}

static uint32_t dir_entry_off(Fat16Fs *fs, Fat16Dir *dir) {
    uint32_t base;
    uint32_t idx;

    idx = dir->index;
    if (dir->cluster == 0) {
        return fs->root_offset + idx * 32UL;
    }
    base = fat16_cluster_off(fs, dir->cluster);
    return base + (idx % (fs->cluster_bytes / 32UL)) * 32UL;
}

static uint16_t dir_entries_per_cluster(Fat16Fs *fs) {
    return (uint16_t)(fs->cluster_bytes / 32);
}

int fat16_dir_open(Fat16Fs *fs, uint16_t dir_cluster, Fat16Dir *dir) {
    dir->start_cluster = dir_cluster;
    dir->cluster = dir_cluster;
    dir->index = 0;
    dir->done = 0;
    if (fs->bytes_per_sec == 0) {
        return FAT16_ERR;
    }
    return FAT16_OK;
}

static void fill_dirent(uint32_t off, Fat16DirEnt *ent) {
    uint8_t raw[11];
    int i;

    i = 0;
    while (i < 11) {
        raw[i] = disk_read8(off + i);
        i = i + 1;
    }
    fat16_format83(raw, ent->name);
    ent->attr = disk_read8(off + 11);
    ent->cluster = disk_read16(off + 26);
    ent->size = disk_read32(off + 28);
}

int fat16_dir_next(Fat16Fs *fs, Fat16Dir *dir, Fat16DirEnt *ent) {
    uint32_t off;
    uint8_t first;
    uint8_t attr;
    uint16_t per;
    uint16_t next;

    if (dir->done) {
        return 0;
    }

    while (1) {
        if (dir->cluster == 0) {
            if (dir->index >= fs->root_entries) {
                dir->done = 1;
                return 0;
            }
        } else {
            per = dir_entries_per_cluster(fs);
            if ((dir->index % per) == 0 && dir->index != 0) {
                next = fat16_fat_next(fs, dir->cluster);
                if (fat16_is_eof(next) || next < 2) {
                    dir->done = 1;
                    return 0;
                }
                dir->cluster = next;
            }
        }

        off = dir_entry_off(fs, dir);
        first = disk_read8(off);
        if (first == 0) {
            dir->done = 1;
            return 0;
        }

        attr = disk_read8(off + 11);
        dir->index = dir->index + 1;

        if (first == 0xE5) {
            continue;
        }
        if (attr == FAT16_ATTR_LFN) {
            continue;
        }
        if ((attr & FAT16_ATTR_VOLUME) && (attr & FAT16_ATTR_DIR) == 0) {
            continue;
        }

        fill_dirent(off, ent);
        return 1;
    }
}

static void copy_dirent(Fat16DirEnt *dst, Fat16DirEnt *src) {
    int i;

    i = 0;
    while (i < 13) {
        dst->name[i] = src->name[i];
        i = i + 1;
    }
    dst->attr = src->attr;
    dst->cluster = src->cluster;
    dst->size = src->size;
}

int fat16_lookup(Fat16Fs *fs, uint16_t dir_cluster, const char *name, Fat16DirEnt *ent) {
    Fat16Dir dir;
    Fat16DirEnt cur;
    char want[11];
    char got[11];
    int i;

    fat16_canon83(name, want);
    fat16_dir_open(fs, dir_cluster, &dir);
    while (fat16_dir_next(fs, &dir, &cur)) {
        fat16_canon83(cur.name, got);
        i = 0;
        while (i < 11) {
            if (want[i] != got[i]) {
                break;
            }
            i = i + 1;
        }
        if (i == 11) {
            copy_dirent(ent, &cur);
            return FAT16_OK;
        }
    }
    return FAT16_ERR_NOTFOUND;
}

static void make_dir_ent(uint16_t cluster, Fat16DirEnt *ent) {
    ent->name[0] = '.';
    ent->name[1] = 0;
    ent->attr = FAT16_ATTR_DIR;
    ent->cluster = cluster;
    ent->size = 0;
}

int fat16_resolve(Fat16Fs *fs, uint16_t start_cluster, const char *path, Fat16DirEnt *ent) {
    char part[13];
    int i;
    int n;
    uint16_t clus;
    int r;

    clus = start_cluster;
    i = 0;
    if (path[0] && is_sep((int)(unsigned char)path[0])) {
        clus = 0;
        i = 1;
    }

    make_dir_ent(clus, ent);

    while (path[i]) {
        while (path[i] && is_sep((int)(unsigned char)path[i])) {
            i = i + 1;
        }
        if (!path[i]) {
            break;
        }

        n = 0;
        while (path[i] && !is_sep((int)(unsigned char)path[i]) && n < 12) {
            part[n] = path[i];
            n = n + 1;
            i = i + 1;
        }
        part[n] = 0;

        if (n == 1 && part[0] == '.') {
            continue;
        }
        if (n == 2 && part[0] == '.' && part[1] == '.') {
            if (clus == 0) {
                make_dir_ent(0, ent);
                continue;
            }
        }

        r = fat16_lookup(fs, clus, part, ent);
        if (r != FAT16_OK) {
            return r;
        }

        if (path[i] && is_sep((int)(unsigned char)path[i])) {
            if ((ent->attr & FAT16_ATTR_DIR) == 0) {
                return FAT16_ERR_NOTDIR;
            }
            clus = ent->cluster;
        } else if (path[i]) {
            /* leftover junk */
            return FAT16_ERR;
        }
    }

    return FAT16_OK;
}

int fat16_read_at(Fat16Fs *fs, uint16_t cluster, uint32_t size,
                  uint32_t offset, uint8_t *buf, uint16_t len) {
    uint32_t remain;
    uint16_t clus;
    uint32_t skip;
    uint32_t pos;
    uint16_t n;
    uint16_t i;
    uint32_t coff;
    uint16_t in_clus;

    if (offset >= size) {
        return 0;
    }
    remain = size - offset;
    if ((uint32_t)len > remain) {
        len = (uint16_t)remain;
    }

    clus = cluster;
    skip = offset;
    while (skip >= fs->cluster_bytes) {
        clus = fat16_fat_next(fs, clus);
        if (fat16_is_eof(clus) || clus < 2) {
            return 0;
        }
        skip = skip - fs->cluster_bytes;
    }

    n = 0;
    pos = skip;
    while (n < len) {
        if (clus < 2 || fat16_is_eof(clus)) {
            break;
        }
        coff = fat16_cluster_off(fs, clus);
        in_clus = (uint16_t)(fs->cluster_bytes - pos);
        i = 0;
        while (n < len && i < in_clus) {
            buf[n] = disk_read8(coff + pos + i);
            n = n + 1;
            i = i + 1;
        }
        if (n < len) {
            clus = fat16_fat_next(fs, clus);
            pos = 0;
        }
    }
    return (int)n;
}

#endif
