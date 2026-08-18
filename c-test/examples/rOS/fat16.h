#ifndef ROS_FAT16_H
#define ROS_FAT16_H

#include <stdint.h>

#define FAT16_ATTR_READONLY 0x01
#define FAT16_ATTR_HIDDEN   0x02
#define FAT16_ATTR_SYSTEM   0x04
#define FAT16_ATTR_VOLUME   0x08
#define FAT16_ATTR_DIR      0x10
#define FAT16_ATTR_ARCHIVE  0x20
#define FAT16_ATTR_LFN      0x0F

#define FAT16_OK            0
#define FAT16_ERR           -1
#define FAT16_ERR_NOTFAT    -2
#define FAT16_ERR_NOTFOUND  -3
#define FAT16_ERR_NOTDIR    -4
#define FAT16_ERR_ISDIR     -5

typedef struct Fat16Fs {
    uint16_t bytes_per_sec;
    uint16_t sec_per_clus;
    uint16_t reserved_sec;
    uint16_t num_fats;
    uint16_t root_entries;
    uint16_t secs_per_fat;
    uint16_t root_secs;
    uint32_t total_secs;
    uint32_t fat_offset;
    uint32_t root_offset;
    uint32_t data_offset;
    uint32_t cluster_bytes;
    uint32_t vol_id;
    char label[12];
} Fat16Fs;

typedef struct Fat16DirEnt {
    char name[13];
    uint8_t attr;
    uint16_t cluster;
    uint32_t size;
} Fat16DirEnt;

typedef struct Fat16Dir {
    uint16_t start_cluster;
    uint16_t cluster;
    uint16_t index;
    uint16_t done;
} Fat16Dir;

void fat16_canon83(const char *in, char out11[11]);
void fat16_format83(const uint8_t raw11[11], char out13[13]);
int fat16_name_eq(const uint8_t raw11[11], const char *user);

int fat16_mount(Fat16Fs *fs);
int fat16_is_eof(uint16_t cluster);
uint16_t fat16_fat_next(Fat16Fs *fs, uint16_t cluster);
uint32_t fat16_cluster_off(Fat16Fs *fs, uint16_t cluster);

int fat16_dir_open(Fat16Fs *fs, uint16_t dir_cluster, Fat16Dir *dir);
int fat16_dir_next(Fat16Fs *fs, Fat16Dir *dir, Fat16DirEnt *ent);

int fat16_lookup(Fat16Fs *fs, uint16_t dir_cluster, const char *name, Fat16DirEnt *ent);
int fat16_resolve(Fat16Fs *fs, uint16_t start_cluster, const char *path, Fat16DirEnt *ent);

int fat16_read_at(Fat16Fs *fs, uint16_t cluster, uint32_t size,
                  uint32_t offset, uint8_t *buf, uint16_t len);

#endif
