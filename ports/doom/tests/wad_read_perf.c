/* A bounded throughput fixture for the texture-reading workload. Reserving
 * 8 MiB without clearing it places the destination in Ripple RAM bank 136,
 * like Doom's zone. No CPU, ISA, storage geometry or bank size is changed. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "doom_build_config.h"
#include "wad_io.h"

static void check(int good, const char *message)
{
    if (!good) {
        printf("WAD READ PERF ERROR %s\n", message);
        exit(1);
    }
}

int main(void)
{
    const uint32_t count = 4096UL;
    WadFile wad;
    void *reserve;
    WadOctet *allocation;
    WadOctet *cursor;
    uint32_t sum = 0;
    uint16_t i;
    check(DOOM_WAD_LENGTH == 4196020UL, "expected shareware fixture");
    check(wad_open_stream(&wad, DOOM_WAD_PATH, DOOM_WAD_LENGTH), "open stream");
    reserve = malloc(8388608UL);
    check(reserve != NULL, "far heap reserve");
    allocation = malloc((size_t)(count + 2UL));
    check(allocation != NULL, "read allocation");
    allocation[0] = 195;
    allocation[count + 1UL] = 195;
    puts("WAD READ PERF begin");
    /* This odd offset is one octet into DOOR2_4. Its expected sum was
     * independently calculated from the original bundled shareware WAD. */
    check(wad_read(&wad, 3830949UL, allocation + 1, count), "texture read");
    puts("WAD READ PERF copied");
    cursor = allocation + 1;
    for (i = 0; i < 4096U; ++i) sum += (uint32_t)*cursor++;
    check(sum == 368674UL, "independent texture checksum");
    check(allocation[0] == 195 && allocation[count + 1UL] == 195,
          "destination guards");
    free(allocation);
    free(reserve);
    wad_close(&wad);
    puts("WAD READ PERF OK 4096 texture octets, far heap, checksum 368674");
    return 0;
}
