/* Native/Ripple fixture for the same octet decoder used by G_ReadDemoTiccmd.
 * Sources: this file and src/wad_io.c. Include src, generated, and the prepared
 * engine headers; define DOOM_PORT=1. Ripple also needs the port/runtime
 * include paths, RIPPLE=1, BANK_SIZE=64000, and a copy of the configured WAD
 * passed as the Rust VM's explicit disk. No Brainfuck execution is needed.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "doom_demo.h"
#include "doom_build_config.h"
#include "wad_io.h"

static void check(const char *name, int good)
{
    if (!good) {
        printf("DEMO FAIL %s\n", name);
        exit(1);
    }
}

static void check_signed_octets(void)
{
    unsigned int i;
    byte raw[5] = {42, 0, 0, 0, 0};
    ticcmd_t cmd;
    uint32_t cursor;
    int expected;
    for (i = 0; i < 256U; ++i) {
        expected = i < 128U ? (int) i : (int) i - 256;
        check("all signed octets", Doom_DemoSigned8(i) == expected);
        raw[1] = i;
        raw[2] = i;
        raw[3] = 255;
        cursor = 0;
        memset(&cmd, 0, sizeof(cmd));
        check("legacy record accepted", Doom_DemoDecodeTic(raw, 4, &cursor, 0, &cmd) == DOOM_DEMO_TIC);
        check("legacy cursor", cursor == 4);
        check("legacy moves", cmd.forwardmove == 42 && cmd.sidemove == expected);
        check("legacy angle", (int32_t) cmd.angleturn == (int32_t) expected * 256L);
        check("unsigned buttons", cmd.buttons == 255U);
        if (i != 128U) {
            raw[0] = i;
            cursor = 0;
            check("forward record accepted", Doom_DemoDecodeTic(raw, 4, &cursor, 0, &cmd) == DOOM_DEMO_TIC);
            check("signed forward", cmd.forwardmove == expected);
            raw[0] = 42;
        }
    }
    /* These inputs also prove the helper masks C cells to serialized octets. */
    check("high C cell bits ignored", Doom_DemoSigned8(511U) == -1);
}

static void check_signed_angles(void)
{
    uint16_t bits[6] = {0, 1, UINT16_C(32767), UINT16_C(32768), UINT16_C(65535), UINT16_C(65280)};
    int32_t expected[6] = {0, 1, INT32_C(32767), -INT32_C(32768), -1, -256};
    byte raw[5] = {255, 128, 0, 0, 127};
    ticcmd_t cmd;
    uint32_t cursor;
    int i;
    for (i = 0; i < 6; ++i) {
        check("signed16 helper", (int32_t) Doom_DemoSigned16(bits[i]) == expected[i]);
        raw[2] = bits[i] & 255U;
        raw[3] = bits[i] >> 8;
        cursor = 0;
        memset(&cmd, 0, sizeof(cmd));
        check("longtic accepted", Doom_DemoDecodeTic(raw, 5, &cursor, 1, &cmd) == DOOM_DEMO_TIC);
        check("longtic fields", cmd.forwardmove == -1 && cmd.sidemove == -128
              && (int32_t) cmd.angleturn == expected[i] && cmd.buttons == 127);
        check("longtic cursor", cursor == 5);
    }
}

static void check_headers(void)
{
    byte raw[13] = {109,2,1,5,0,0,0,0,0,1,0,0,0};
    DoomDemoHeader header;
    uint32_t length;
    for (length = 0; length < 13UL; ++length)
        check("short header", Doom_DemoDecodeHeader(raw, length, 109, &header) != NULL);
    check("vanilla header", Doom_DemoDecodeHeader(raw, 13, 109, &header) == NULL);
    check("header fields", header.skill == 2 && header.episode == 1 && header.map == 5
          && header.consoleplayer == 0 && header.playeringame[0] && !header.longtics);
    raw[0] = 111;
    check("longtics header", Doom_DemoDecodeHeader(raw, 13, 109, &header) == NULL && header.longtics);
    raw[0] = 110;
    check("version rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    raw[0] = 109;
    raw[1] = 5;
    check("skill rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    raw[1] = 2;
    raw[2] = 0;
    check("episode rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    raw[2] = 1;
    raw[3] = 0;
    check("map rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    raw[3] = 5;
    raw[4] = 3;
    check("deathmatch rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    raw[4] = 0;
    raw[5] = 2;
    check("game flags rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    raw[5] = 0;
    raw[8] = 4;
    check("console player rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    raw[8] = 0;
    raw[9] = 2;
    check("player flag rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    raw[9] = 0;
    check("absent console rejected", Doom_DemoDecodeHeader(raw, 13, 109, &header) != NULL);
    check("null header input", Doom_DemoDecodeHeader(NULL, 13, 109, &header) != NULL);
    check("null header output", Doom_DemoDecodeHeader(raw, 13, 109, NULL) != NULL);
}

static void check_bounds(void)
{
    byte raw[6] = {1,2,3,4,5,128};
    ticcmd_t before, cmd;
    uint32_t length, cursor;
    int longtics;
    memset(&before, 0, sizeof(before));
    before.forwardmove = 11;
    before.sidemove = -12;
    before.angleturn = 1234;
    before.buttons = 99;
    for (longtics = 0; longtics <= 1; ++longtics) {
        for (length = 0; length < (longtics ? 5UL : 4UL); ++length) {
            cmd = before;
            cursor = 0;
            check("short record rejected", Doom_DemoDecodeTic(raw, length, &cursor, longtics, &cmd) == DOOM_DEMO_ERROR);
            check("short record untouched", cursor == 0 && memcmp(&before, &cmd, sizeof(cmd)) == 0);
        }
    }
    cmd = before;
    cursor = 5;
    check("end marker accepted", Doom_DemoDecodeTic(raw, 6, &cursor, 0, &cmd) == DOOM_DEMO_END);
    check("end marker untouched", cursor == 5 && memcmp(&before, &cmd, sizeof(cmd)) == 0);
    cursor = 6;
    check("at end rejected", Doom_DemoDecodeTic(raw, 6, &cursor, 0, &cmd) == DOOM_DEMO_ERROR);
    cursor = UINT32_MAX;
    check("oversized cursor rejected", Doom_DemoDecodeTic(raw, 6, &cursor, 0, &cmd) == DOOM_DEMO_ERROR);
    check("oversized cursor unchanged", cursor == UINT32_MAX);
    cursor = 4;
    check("short trailing record rejected", Doom_DemoDecodeTic(raw, 6, &cursor, 0, &cmd) == DOOM_DEMO_ERROR);
    check("null tic input", Doom_DemoDecodeTic(NULL, 6, &cursor, 0, &cmd) == DOOM_DEMO_ERROR);
    check("null cursor", Doom_DemoDecodeTic(raw, 6, NULL, 0, &cmd) == DOOM_DEMO_ERROR);
    check("null command", Doom_DemoDecodeTic(raw, 6, &cursor, 0, NULL) == DOOM_DEMO_ERROR);
}

static uint32_t hash_word(uint32_t hash, uint32_t word)
{
    int i;
    for (i = 0; i < 4; ++i) {
        hash = (hash ^ (word & UINT32_C(255))) * UINT32_C(16777619);
        word >>= 8;
    }
    return hash;
}

static void check_real_demo(void)
{
    WadFile wad;
    WadLump lump;
    WadOctet *raw;
    DoomDemoHeader header;
    ticcmd_t cmd;
    uint32_t cursor, count = 0, hash = UINT32_C(2166136261);
    int result;
    check("open configured IWAD", wad_open(&wad, DOOM_WAD_PATH, DOOM_WAD_LENGTH));
    check("find DEMO1", wad_find(&wad, "DEMO1", &lump));
    check("canonical demo length", lump.length == 20118UL);
    raw = malloc((size_t) lump.length);
    check("demo allocation", raw != NULL);
    check("read DEMO1", wad_read_lump(&wad, &lump, 0, raw, lump.length));
    check("real header", Doom_DemoDecodeHeader(raw, lump.length, 109, &header) == NULL);
    check("real map fields", header.skill == 2 && header.episode == 1 && header.map == 5 && !header.longtics);
    cursor = DOOM_DEMO_HEADER_OCTETS;
    memset(&cmd, 0, sizeof(cmd));
    while ((result = Doom_DemoDecodeTic(raw, lump.length, &cursor, header.longtics, &cmd)) == DOOM_DEMO_TIC) {
        if (count < 6UL)
            check("first six idle tics", cmd.forwardmove == 0 && cmd.sidemove == 0 && cmd.angleturn == 0 && cmd.buttons == 0);
        if (count == 6UL)
            check("first real signed movement", cmd.forwardmove == -4 && cmd.sidemove == 0 && cmd.angleturn == 0 && cmd.buttons == 0);
        hash = hash_word(hash, (uint32_t) (int32_t) cmd.forwardmove);
        hash = hash_word(hash, (uint32_t) (int32_t) cmd.sidemove);
        hash = hash_word(hash, (uint32_t) (int32_t) cmd.angleturn);
        hash = hash_word(hash, (uint32_t) cmd.buttons);
        ++count;
    }
    check("real end marker", result == DOOM_DEMO_END && cursor == 20117UL);
    check("real tic count", count == 5026UL);
    check("real signed command hash", hash == UINT32_C(0x9ca0bb8e));
    free(raw);
    wad_close(&wad);
}

int main(void)
{
    check_signed_octets();
    check_signed_angles();
    check_headers();
    check_bounds();
    check_real_demo();
    puts("DEMO OK signed octets, bounded records, DEMO1 5026 9ca0bb8e");
    return 0;
}
