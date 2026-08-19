#ifndef ROS_LOADER_C
#define ROS_LOADER_C

#include "loader.h"
#include "imem.h"
#include "disk.h"
#include <stdio.h>

static uint16_t le16(unsigned char *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint32_t le32(unsigned char *p) {
    uint32_t lo;
    uint32_t hi;

    lo = le16(p);
    hi = le16(p + 2);
    return lo | (hi << 16);
}

int sys1_parse(unsigned char *raw, Sys1Header *hdr) {
    if (raw[0] != SYS1_MAGIC0 || raw[1] != SYS1_MAGIC1 ||
        raw[2] != SYS1_MAGIC2 || raw[3] != SYS1_MAGIC3) {
        return -1;
    }
    hdr->bank_size = le16(raw + 4);
    hdr->code_bank = le16(raw + 6);
    hdr->gp_bank = le16(raw + 8);
    hdr->sb_bank = le16(raw + 10);
    hdr->entry = le32(raw + 12);
    hdr->insn_count = le32(raw + 16);
    hdr->data_size = le32(raw + 20);
    return 0;
}

/* Walk the file once. Restarting fat16_read_at per instruction is O(n^2). */
static uint8_t seq_get(Fat16Fs *fs, uint16_t *clus, uint32_t *pos) {
    uint32_t coff;

    if (*pos >= fs->cluster_bytes) {
        *clus = fat16_fat_next(fs, *clus);
        *pos = 0;
    }
    coff = fat16_cluster_off(fs, *clus);
    *pos = *pos + 1;
    return disk_read8(coff + (*pos - 1));
}

/* Separate from sys1_load_and_enter so the inner STORC loop is not
 * drowning in that function's spill slots (tens of thousands of insns).
 * STORC's addr is 16-bit; walk PCB when `i` reaches bank_size (COMMAND.COM
 * is larger than one bank). */
static void load_code(Fat16Fs *fs, uint16_t *clus, uint32_t *pos,
                     unsigned bank, uint16_t bank_size, uint32_t nins) {
    unsigned char ib[8];
    unsigned cells[4];
    uint32_t i;
    unsigned j;
    unsigned addr;

    i = 0;
    addr = 0;
    while (i < nins) {
        j = 0;
        while (j < 8) {
            ib[j] = seq_get(fs, clus, pos);
            j = j + 1;
        }
        cells[0] = ib[0];
        cells[1] = le16(ib + 2);
        cells[2] = le16(ib + 4);
        cells[3] = le16(ib + 6);
        imem_store(bank, addr, cells);
        i = i + 1;
        addr = addr + 1;
        if (addr == bank_size) {
            addr = 0;
            bank = bank + 1;
        }
    }
}

int sys1_load_and_enter(Fat16Fs *fs, Fat16DirEnt *ent) {
    unsigned char raw[24];
    Sys1Header hdr;
    uint32_t i;
    int n;
    uint16_t clus;
    uint32_t pos;

    n = fat16_read_at(fs, ent->cluster, ent->size, 0, raw, SYS1_HEADER_SIZE);
    if (n != SYS1_HEADER_SIZE) {
        puts("SYS header short");
        return -1;
    }
    if (sys1_parse(raw, &hdr) != 0) {
        puts("SYS magic");
        return -1;
    }
    clus = ent->cluster;
    pos = SYS1_HEADER_SIZE;
    while (pos >= fs->cluster_bytes) {
        clus = fat16_fat_next(fs, clus);
        pos = pos - fs->cluster_bytes;
    }

    load_code(fs, &clus, &pos, hdr.code_bank, hdr.bank_size, hdr.insn_count);

    i = 0;
    while (i < hdr.data_size) {
        dmem_store(hdr.gp_bank, (unsigned)i, seq_get(fs, &clus, &pos));
        i = i + 1;
    }

    ros_enter(hdr.code_bank, (unsigned)hdr.entry, hdr.gp_bank, hdr.sb_bank);
    return -1;
}

#endif
