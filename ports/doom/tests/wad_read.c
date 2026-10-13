/* The expected octets and bulk checksums below come from an independent
 * host read of the bundled shareware WAD, not another call to wad_read.
 * The small variant omits bulk reads so the same edge checks fit a bounded
 * Brainfuck run. The complete variant crosses both storage and C RAM banks. */
#include <stdio.h>
#include <stdlib.h>
#include "doom_build_config.h"
#include "wad_io.h"

static const WadOctet header[] = {
    73, 87, 65, 68, 240, 4, 0, 0, 180, 183, 63, 0,
    0, 0, 0, 31, 23, 11, 23, 15, 7, 75, 75, 75
};
static const WadOctet first_boundary[] = {
    0, 32, 0, 0, 0, 26, 0, 0, 0, 25, 0, 0,
    0, 25, 0, 0, 0, 25, 0, 0, 0, 2, 0, 2
};
static const WadOctet second_boundary[] = {
    0, 0, 0, 0, 0, 214, 1, 255, 255, 99, 1, 32, 1, 1, 0, 0
};
static const WadOctet final_octets[] = {
    78, 68, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 70, 95, 69, 78, 68, 0, 0, 0
};

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("WAD READ ERROR %s\n", message);
        exit(1);
    }
}

static void read_case(WadFile *wad, uint32_t offset,
                      const WadOctet *expected, uint16_t count)
{
    WadOctet actual[26];
    uint16_t i;
    for (i = 0; i < 26; ++i) actual[i] = 195;
    check(wad_read(wad, offset, actual + 1, count), "short read");
    check(actual[0] == 195 && actual[count + 1] == 195, "read guards");
    for (i = 0; i < count; ++i)
        check(actual[i + 1] == expected[i], "serialized octet differs");
}

static void check_decoders(void)
{
    WadOctet octets[5];
    uint16_t narrow = 0;
    uint32_t wide = 0;
    octets[0] = 255;
    /* On Ripple these retain nonzero high bits in each 16-bit C cell. Native
     * unsigned char truncates them, producing the same serialized octets. */
    octets[1] = (WadOctet)0xab12U;
    octets[2] = (WadOctet)0xcd34U;
    octets[3] = (WadOctet)0xef56U;
    octets[4] = (WadOctet)0x9878U;
    check(wad_u16(octets, 5UL, 1UL, &narrow) && narrow == 0x3412U,
          "16-bit decoder masks");
    check(wad_u32(octets, 5UL, 1UL, &wide) && wide == 0x78563412UL,
          "32-bit decoder masks");
    check(!wad_u16(octets, 5UL, 4UL, &narrow) && narrow == 0x3412U,
          "truncated 16-bit field");
    check(!wad_u32(octets, 5UL, 2UL, &wide) && wide == 0x78563412UL,
          "truncated 32-bit field");
    check(!wad_u16(octets, 5UL, UINT32_MAX, &narrow), "16-bit offset wrap");
    check(!wad_u32(octets, 5UL, UINT32_MAX, &wide), "32-bit offset wrap");
    check(wad_range(UINT32_MAX, UINT32_MAX, 0UL), "empty maximum range");
    check(!wad_range(UINT32_MAX, UINT32_MAX - 1UL, 3UL), "range sum wrap");
}

#ifndef WAD_READ_SMALL
static void bulk_case(WadFile *wad, uint32_t offset,
                      uint32_t expected_sum, uint32_t expected_weighted)
{
    const uint32_t count = 65540UL;
    WadOctet *allocation = malloc((size_t)(count + 2UL));
    WadOctet *data;
    uint32_t i;
    uint32_t sum = 0;
    uint32_t weighted = 0;
    check(allocation != NULL, "bulk allocation");
    data = allocation + 1;
    allocation[0] = 195;
    data[count] = 195;
    /* Any contiguous 65,540-cell destination crosses a 64,000-cell RAM bank.
     * Both file offsets also cross 32,768-octet chunks and a 65,536-octet
     * storage block; offset one exercises odd chunk starts and tails. */
    check(wad_read(wad, offset, data, count), "bulk read");
    check(allocation[0] == 195 && data[count] == 195, "bulk guards");
    for (i = 0; i < count; ++i) {
        check(data[i] <= 255U, "bulk octet width");
        sum += (uint32_t)data[i];
        weighted += (uint32_t)data[i] * ((i & 255UL) + 1UL);
    }
    check(sum == expected_sum && weighted == expected_weighted,
          "bulk independent checksums");
    free(allocation);
}
#endif

int main(void)
{
    WadFile wad;
    WadOctet guard = 195;
    check(DOOM_WAD_LENGTH == 4196020UL, "expected shareware fixture");
    check(!wad_open_stream(&wad, DOOM_WAD_PATH, 11UL), "truncated stream");
    wad_close(&wad);
#ifndef RIPPLE
    check(!wad_open_stream(&wad, DOOM_WAD_PATH, DOOM_WAD_LENGTH + 1UL),
          "host physical length");
    wad_close(&wad);
#endif
    check(wad_open_stream(&wad, DOOM_WAD_PATH, DOOM_WAD_LENGTH), "open stream");
    check(wad.lumps == NULL && wad.lump_count == 0 && wad.directory_offset == 0,
          "stream does not build an index");
    read_case(&wad, 0UL, header, 24);
    read_case(&wad, 1UL, header + 1, 11);
    read_case(&wad, 1UL, header + 1, 10);
    read_case(&wad, 4UL, header + 4, 1);
    read_case(&wad, 5UL, header + 5, 1);
    read_case(&wad, 65528UL, first_boundary, 24);
    read_case(&wad, 65535UL, first_boundary + 7, 2);
    read_case(&wad, 65535UL, first_boundary + 7, 1);
    read_case(&wad, 65536UL, first_boundary + 8, 15);
    read_case(&wad, 131067UL, second_boundary, 16);
    read_case(&wad, DOOM_WAD_LENGTH - 20UL, final_octets, 20);
    read_case(&wad, DOOM_WAD_LENGTH - 1UL, final_octets + 19, 1);
    check(wad_read(&wad, 0UL, NULL, 0UL), "empty null read");
    check(wad_read(&wad, DOOM_WAD_LENGTH, NULL, 0UL), "empty EOF read");
    check(!wad_read(&wad, 0UL, NULL, 1UL), "nonempty null read");
    check(!wad_read(&wad, DOOM_WAD_LENGTH, &guard, 1UL), "read past EOF");
    check(!wad_read(&wad, DOOM_WAD_LENGTH + 1UL, NULL, 0UL), "empty past EOF");
    check(!wad_read(&wad, UINT32_MAX, &guard, 2UL), "read offset wrap");
    check(!wad_read(&wad, 1UL, &guard, UINT32_MAX), "read length wrap");
    check(guard == 195, "rejected read changed destination");
    check_decoders();
#ifndef WAD_READ_SMALL
    bulk_case(&wad, 0UL, 3480223UL, 453822427UL);
    bulk_case(&wad, 1UL, 3480150UL, 453830716UL);
#endif
    wad_close(&wad);
    check(wad.host_file == NULL && wad.lumps == NULL && wad.length == 0,
          "close stream");
#ifdef WAD_READ_SMALL
    puts("WAD READ SMALL OK odd octets, storage boundaries, bounds, decoder masks");
#else
    puts("WAD READ OK odd octets, storage/RAM boundaries, bounds, decoder masks");
#endif
    return 0;
}
