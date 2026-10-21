/* Run the production parser/selector under host UBSan, using ONLY regular
 * disposable fixtures emitted by boot.py test --nvme-partition. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../boot/root-window.h"
static uint64_t size;
static int read_disk(void *ctx, uint64_t at, void *out, size_t bytes) {
    assert(at <= size && bytes <= size - at);
    return fseeko(ctx, (off_t)at, SEEK_SET) || fread(out, 1, bytes, ctx) != bytes ? -1 : 0;
}
int main(int argc, char **argv) {
    assert(argc == 3 || argc == 4);
    unsigned sector = (unsigned)strtoul(argv[2], 0, 10);
    const char *reserved_env = getenv("RIPLIX_TEST_RESERVATION");
    uint64_t reserved = reserved_env ? strtoull(reserved_env, 0, 10) : 16777216;
    assert(sector == 512 || sector == 4096);
    struct stat info;
    assert(!lstat(argv[1], &info) && S_ISREG(info.st_mode));
    FILE *file = fopen(argv[1], "rb"); assert(file);
    size = (uint64_t)info.st_size;
    RootIdentity id = {
        .disk={0x78,0x56,0x34,0x12,0x34,0x12,0x78,0x56,0x9a,0xbc,0x12,0x34,0x56,0x78,0x9a,0xbc},
        .partition={0x29,0xf3,0x46,0x59,0x99,0x8d,0x33,0x43,0xb3,0x87,0x04,0xea,0x4f,0xb7,0x09,0x4b},
        .first=4194304 / sector, .last=(4194304 + reserved) / sector - 1, .sector=sector
    };
    RootWindow window = {.ready=1};
    const char *error = root_window_open(read_disk, file, size, sector, &id, &window);
    if (argc == 4) {
        assert(error && !strcmp(error, argv[3]));
        assert(!window.offset && !window.bytes && !window.ready);
    } else {
        if (error) { fprintf(stderr, "%s\n", error); abort(); }
        assert(window.offset == 4194304 && window.bytes == 16777216 && window.ready);
        uint64_t native;
        assert(!root_window_translate(&window, 0, 4096, &native) && native == 4194304);
        assert(!root_window_translate(&window, window.bytes - 1, 1, &native));
        assert(root_window_contains(&window, native, 1));
        assert(root_window_translate(&window, window.bytes - 1, 2, &native));
        assert(root_window_translate(&window, UINT64_MAX, 2, &native));
        assert(!root_window_contains(&window, UINT64_MAX, 4096));
        assert(!root_window_contains(&window, window.offset - 1, 1));
        window.ready = 0;
        assert(root_window_translate(&window, 0, 0, &native));
    }
    fclose(file); puts("ROOT_WINDOW_UBSAN_OK");
}
