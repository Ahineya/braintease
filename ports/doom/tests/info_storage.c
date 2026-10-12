/* Canonical info-table equivalence, shared by native C and Ripple.
 * Sources: this file plus generated info_fill_*.c and info_init.c.
 * Define DOOM_GENERATED_INFO=1 and DOOM_PORT=1; include src, generated and
 * prepared engine headers. Native additionally includes original aggregate
 * initializers from info.c below and compares every field and callback.
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "info.h"
#include "p_mobj.h"
#include "info_actions.h"
#include "info_test_actions.h"

static int action_seen;
#define DEFINE_ACTION(id, name) void name(void) { action_seen = id; }
DOOM_INFO_ACTIONS(DEFINE_ACTION)
#undef DEFINE_ACTION

#ifndef RIPPLE
/* Compile the untouched initializer expressions as the independent native
 * aggregate reference. The production info.c suppresses them with this flag. */
#undef DOOM_GENERATED_INFO
#define states canonical_states
#define mobjinfo canonical_mobjinfo
#define sprnames canonical_sprnames
#include "info.c"
#undef states
#undef mobjinfo
#undef sprnames
#define DOOM_GENERATED_INFO 1
#endif

static void check(const char *name, int good)
{
    if (!good) {
        printf("INFO FAIL %s\n", name);
        exit(1);
    }
}

static unsigned int action_number(actionf_v action)
{
    if (action == NULL) return 0;
#define FIND_ACTION(id, name) if (action == name) return id;
    DOOM_INFO_ACTIONS(FIND_ACTION)
#undef FIND_ACTION
    check("recognized action pointer", 0);
    return 0;
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

static uint32_t hash_states(void)
{
    uint32_t hash = UINT32_C(2166136261);
    int i;
    for (i = 0; i < NUMSTATES; ++i) {
        state_t *state = &states[i];
#ifndef RIPPLE
#define SAME(field) check("canonical state " #field, state->field == canonical_states[i].field)
        SAME(sprite); SAME(frame); SAME(tics); SAME(action.acv);
        SAME(nextstate); SAME(misc1); SAME(misc2);
#undef SAME
#endif
        hash = hash_word(hash, (uint32_t) state->sprite);
        /* The frame stores a 16-bit fullbright flag in a target int cell. */
        hash = hash_word(hash, (uint32_t) (uint16_t) state->frame);
        hash = hash_word(hash, (uint32_t) (int32_t) state->tics);
        hash = hash_word(hash, (uint32_t) action_number(state->action.acv));
        hash = hash_word(hash, (uint32_t) state->nextstate);
        hash = hash_word(hash, (uint32_t) (int32_t) state->misc1);
        hash = hash_word(hash, (uint32_t) (int32_t) state->misc2);
    }
    return hash;
}

static uint32_t hash_mobjs(void)
{
    uint32_t hash = UINT32_C(2166136261);
    int i;
    for (i = 0; i < NUMMOBJTYPES; ++i) {
        mobjinfo_t *info = &mobjinfo[i];
#ifndef RIPPLE
#define SAME(field) check("canonical mobj " #field, info->field == canonical_mobjinfo[i].field)
        SAME(doomednum); SAME(spawnstate); SAME(spawnhealth); SAME(seestate);
        SAME(seesound); SAME(reactiontime); SAME(attacksound); SAME(painstate);
        SAME(painchance); SAME(painsound); SAME(meleestate); SAME(missilestate);
        SAME(deathstate); SAME(xdeathstate); SAME(deathsound); SAME(speed);
        SAME(radius); SAME(height); SAME(mass); SAME(damage); SAME(activesound);
        SAME(flags); SAME(raisestate);
#undef SAME
#endif
#define HASH(field) hash = hash_word(hash, (uint32_t) (int32_t) info->field)
        HASH(doomednum); HASH(spawnstate); HASH(spawnhealth); HASH(seestate);
        HASH(seesound); HASH(reactiontime); HASH(attacksound); HASH(painstate);
        HASH(painchance); HASH(painsound); HASH(meleestate); HASH(missilestate);
        HASH(deathstate); HASH(xdeathstate); HASH(deathsound); HASH(speed);
        HASH(radius); HASH(height); HASH(mass); HASH(damage); HASH(activesound);
        hash = hash_word(hash, info->flags);
        HASH(raisestate);
#undef HASH
    }
    return hash;
}

int main(void)
{
    void *bank_padding;
    state_t *original_states;
    mobjinfo_t *original_mobjs;
    uint32_t state_hash, mobj_hash;
    check("initial null tables", states == NULL && mobjinfo == NULL);
    /* A fresh Ripple heap begins at bank 5 offset 0. This allocation places
     * states near offset 60000, so its 7736-cell payload crosses bank 6. */
    bank_padding = malloc(60000UL);
    check("bank boundary padding", bank_padding != NULL);
    check("allocate info tables", Doom_InitInfo());
    original_states = states;
    original_mobjs = mobjinfo;
    check("idempotent initialization", Doom_InitInfo()
          && states == original_states && mobjinfo == original_mobjs);
    state_hash = hash_states();
    mobj_hash = hash_mobjs();
    /* Hashes of the original native aggregate reference, in field order,
     * with action pointers normalized to the generated stable action IDs. */
    check("canonical state hash", state_hash == UINT32_C(0xa44a5ccd));
    check("canonical object hash", mobj_hash == UINT32_C(0xfc4d5f58));
    check("null state", states[S_NULL].tics == -1 && states[S_NULL].action.acv == NULL);
    check("fullbright state", (uint16_t) states[S_PISTOLFLASH].frame == UINT16_C(32768));
    check("player dimensions", mobjinfo[MT_PLAYER].radius == 16L * FRACUNIT
          && mobjinfo[MT_PLAYER].height == 56L * FRACUNIT);
    check("projectile fixed speed", mobjinfo[MT_ROCKET].speed == 20L * FRACUNIT);
    check("wide mass", mobjinfo[MT_KEEN].mass == INT32_C(10000000)
          && mobjinfo[MT_BOSSBRAIN].mass == INT32_C(10000000));
    check("high monster flag", (mobjinfo[MT_POSSESSED].flags & MF_COUNTKILL) != 0);
    action_seen = 0;
    states[S_PUNCH2].action.acv();
    check("callable action pointer", action_seen == (int) action_number(A_Punch));
    states[S_NULL].tics = 123;
    check("initialization preserves live mutations", Doom_InitInfo() && states[S_NULL].tics == 123);
    Doom_FreeInfo();
    check("free clears pointers", states == NULL && mobjinfo == NULL);
    check("reinitialize tables", Doom_InitInfo());
    check("reinitialize restores canonical data", states[S_NULL].tics == -1
          && mobjinfo[MT_ROCKET].speed == 20L * FRACUNIT);
    Doom_FreeInfo();
    free(bank_padding);
    puts("INFO OK 967 states, 137 objects, canonical fields and callbacks");
    return 0;
}
