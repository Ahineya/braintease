/* SPDX-License-Identifier: GPL-2.0-only */
struct sysinfoblk {
    uint8_t infosize; /* For expandability */
    uint8_t banks;    /* Banks in 64K (and thus pagesize) */
    uint8_t max_open;
    uint8_t nproc;   /* Number of processes */
    uint16_t ticks;  /* Tick rate in HZ */
    uint16_t memk;   /* Memory in KB */
    uint16_t usedk;  /* Used memory in KB */
    uint16_t config; /* Config flag mask */
#define CONF_PROFIL 1
#define CONF_NET 2 /* Hah.. 8) */
#define CONF_LEVEL_2 4
    uint16_t loadavg[3];
    uint16_t swapk;
    uint16_t swapusedk;
    uint8_t cputype; /* CPU type information */
    uint8_t cpu[3];  /* CPU type specific data */
    uint16_t spare[8];
    char uname[64];
};
