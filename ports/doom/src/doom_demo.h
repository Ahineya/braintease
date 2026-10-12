#ifndef DOOM_PORT_DEMO_H
#define DOOM_PORT_DEMO_H

#include <stddef.h>
#include <stdint.h>
#include "d_ticcmd.h"

#define DOOM_DEMO_HEADER_OCTETS 13UL
#define DOOM_DEMO_ERROR (-1)
#define DOOM_DEMO_END 0
#define DOOM_DEMO_TIC 1

typedef struct {
    int skill, episode, map, deathmatch;
    int respawn, fast, nomonsters, consoleplayer;
    int playeringame[4];
    int longtics;
} DoomDemoHeader;

/* Casts to signed char do not sign-extend serialized octets when CHAR_BIT=16. */
static inline int Doom_DemoSigned8(unsigned int octet)
{
    int value = (int) (octet & 255U);
    return value < 128 ? value : value - 256;
}

static inline int16_t Doom_DemoSigned16(uint16_t bits)
{
    int32_t value = (int32_t) bits;
    if (bits & UINT16_C(32768)) value -= INT32_C(65536);
    return (int16_t) value;
}

static inline const char *Doom_DemoDecodeHeader(const byte *raw, uint32_t length,
                                              int expected_version, DoomDemoHeader *header)
{
    unsigned int version;
    unsigned int i;
    if (raw == NULL || header == NULL || length < DOOM_DEMO_HEADER_OCTETS)
        return "Truncated demo header";
    version = raw[0] & 255U;
    if (version != (unsigned int) expected_version && version != 111U)
        return "Unsupported demo version";
    if ((raw[1] & 255U) > 4U || !(raw[2] & 255U) || (raw[2] & 255U) > 4U
        || !(raw[3] & 255U) || (raw[3] & 255U) > 32U)
        return "Invalid demo skill or map";
    if ((raw[4] & 255U) > 2U || (raw[5] & 255U) > 1U
        || (raw[6] & 255U) > 1U || (raw[7] & 255U) > 1U)
        return "Invalid demo game flags";
    if ((raw[8] & 255U) >= 4U) return "Invalid demo console player";
    for (i = 0; i < 4; ++i)
        if ((raw[9 + i] & 255U) > 1U) return "Invalid demo player flags";
    if (!(raw[9 + (raw[8] & 255U)] & 255U)) return "Demo console player is absent";
    header->skill = raw[1] & 255U;
    header->episode = raw[2] & 255U;
    header->map = raw[3] & 255U;
    header->deathmatch = raw[4] & 255U;
    header->respawn = raw[5] & 255U;
    header->fast = raw[6] & 255U;
    header->nomonsters = raw[7] & 255U;
    header->consoleplayer = raw[8] & 255U;
    for (i = 0; i < 4; ++i) header->playeringame[i] = raw[9 + i] & 255U;
    header->longtics = version == 111U;
    return NULL;
}

/* Only commit a command/cursor after the complete four- or five-octet record
 * is available. The end marker leaves both unchanged, as the engine expects. */
static inline int Doom_DemoDecodeTic(const byte *raw, uint32_t length,
                                    uint32_t *cursor, int longtics, ticcmd_t *cmd)
{
    uint32_t at, count;
    uint16_t angle;
    if (raw == NULL || cursor == NULL || cmd == NULL) return DOOM_DEMO_ERROR;
    at = *cursor;
    if (at >= length) return DOOM_DEMO_ERROR;
    if ((raw[at] & 255U) == 128U) return DOOM_DEMO_END;
    count = longtics ? 5UL : 4UL;
    if (count > length - at) return DOOM_DEMO_ERROR;
    if (longtics)
        angle = (uint16_t) ((raw[at + 2UL] & 255U)
              | (uint16_t) ((uint16_t) (raw[at + 3UL] & 255U) << 8));
    else
        angle = (uint16_t) ((uint16_t) (raw[at + 2UL] & 255U) << 8);
    cmd->forwardmove = (signed char) Doom_DemoSigned8(raw[at]);
    cmd->sidemove = (signed char) Doom_DemoSigned8(raw[at + 1UL]);
    cmd->angleturn = Doom_DemoSigned16(angle);
    cmd->buttons = raw[at + count - 1UL] & 255U;
    *cursor = at + count;
    return DOOM_DEMO_TIC;
}

#endif
