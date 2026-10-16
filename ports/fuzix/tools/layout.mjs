// C16 V2: 1,024 native characters per block, 32 native characters per inode.
export const CELLS = 1024;
export const BLOCK = CELLS * 2;
export const INODES_PER_BLOCK = CELLS / 32;
export const MAGIC = 0x7253;
export const SIGNATURE = 'RIPPLE-FUZIX-C16-V2';
export const inodeBase = ino => (2 + Math.floor(ino / INODES_PER_BLOCK)) * CELLS +
                               (ino % INODES_PER_BLOCK) * 32;
export const DISK_GEOMETRY = {totalSectors: 524288, sectorsPerFat: 256,
                              sectorsPerCluster: 16};
