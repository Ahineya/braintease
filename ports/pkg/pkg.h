#ifndef RIPLIX_PKG_H
#define RIPLIX_PKG_H
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#define PKG_FILES 128
#define PKG_ENTRIES 1024
#define PKG_INDEX_LIMIT 1048576UL
#define PKG_PATH 256
#define PKG_LIMIT 536870912UL
typedef unsigned char PkgByte; /* A wire octet, including when CHAR_BIT is 16. */
typedef struct {
    uint32_t h[8];
    uint64_t count;
    unsigned used;
    PkgByte block[64];
} PkgSha;
typedef struct {
    char path[PKG_PATH], hash[65];
    unsigned long size;
    unsigned mode;
} PkgFile;
typedef struct {
    char name[25], version[32], target[64], depends[256], hash[65];
    unsigned count;
    PkgFile files[PKG_FILES];
} Package;
extern char pkg_error[256];
extern int pkg_adopt_seed, pkg_discard_previous;
void pkg_sha_init(PkgSha *);
void pkg_sha_update(PkgSha *, const PkgByte *, size_t);
void pkg_sha_end(PkgSha *, char out[65]);
int pkg_hash_file(const char *, char out[65], unsigned long *);
int pkg_fail(const char *, const char *);
int pkg_digest_ok(const char *);
int pkg_token(const char *, size_t);
int pkg_path_ok(const char *);
int pkg_join(char out[512], const char *, const char *);
int pkg_mkdirs(const char *);
int pkg_copy(const char *, const char *, unsigned);
int pkg_remove_tree(const char *);
int pkg_write_meta(FILE *, const Package *, const char *magic);
int pkg_read_meta(FILE *, Package *, const char *magic);
int pkg_create(const char *, const char *, const char *);
int pkg_inspect(const char *, Package *, const char *extract);
int pkg_install(const char *, const char *, const char *);
int pkg_remove(const char *, const char *);
int pkg_recover(const char *);
int pkg_query(const char *, const char *);
int pkg_repo(const char *);
int pkg_db_write(const char *, const Package *);
int pkg_db_read(const char *, Package *);
int pkg_clean(const char *);
int pkg_verify(const char *, const char *, const char *);
int pkg_download(const char *, const char *, unsigned long);
int pkg_update(const char *);
int pkg_search(const char *, const char *, const char *);
int pkg_add(const char *, const char *, const char *);
int pkg_cli(int, char **);
#endif
