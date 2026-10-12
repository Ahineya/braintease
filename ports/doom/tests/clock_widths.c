/* Isolated native clock/loop regression. Build with -DDOOM_PORT=1 and the
 * prepared engine include directories, plus dead-section elimination as in
 * map_decode.c. No extra engine sources are needed: the loop is included to
 * check private clock conversion and tic-buffer counters as well as its API.
 * This is a synthetic clock, never the Brainfuck backend or host wall clock.
 */
#include <assert.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "d_loop.c"

boolean net_client_connected;
boolean drone;
static int32_t logical_tick;
static int32_t grants;
static int32_t builds;
static int32_t runs;
static int32_t reads;

int32_t I_GetTime(void)
{
    ++reads;
    return logical_tick;
}

int32_t I_GetTimeMS(void)
{
    ++reads;
    return logical_tick / 35 * 1000 + logical_tick % 35 * 1000 / 35;
}

void Doom_NextTick(void)
{
    ++logical_tick;
    ++grants;
}

void I_StartTic(void) { }
void I_Sleep(int milliseconds)
{
    (void) milliseconds;
    assert(!"the single-player logical loop must never wait for wall time");
}
void I_Error(char *format, ...)
{
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    abort();
}

static void NoEvents(void) { }
static void BuildCommand(ticcmd_t *cmd, int32_t tic)
{
    assert(tic == builds);
    ++builds;
    cmd->forwardmove = (signed char) (tic % 17);
}
static void RunCommand(ticcmd_t *cmds, boolean *ingame)
{
    assert(ingame[0]);
    assert(cmds[0].forwardmove == runs % 17);
    assert(logical_tick == runs + 1);
    ++runs;
}

int main(void)
{
    int32_t tick;
    size_t cell;
    ticcmd_set_t *saved_ticdata;
    loop_interface_t callbacks = {NoEvents, BuildCommand, RunCommand, NoEvents};
    /* First test exact conversion and pure reads around the 16-bit boundary. */
    for (tick = 0; tick < 66000; ++tick)
    {
        logical_tick = tick;
        assert(GetAdjustedTime() == tick);
        assert(I_GetTime() == tick);
        assert(I_GetTimeMS() == tick / 35 * 1000 + tick % 35 * 1000 / 35);
        assert(logical_tick == tick && grants == 0);
    }
    logical_tick = 0;
    ticdup = 1;
    assert(ticdata == NULL);
    D_RegisterLoopCallbacks(&callbacks);
    assert(ticdata != NULL);
    for (cell = 0; cell < (size_t) BACKUPTICS * sizeof(*ticdata); ++cell)
        assert(((unsigned char *) ticdata)[cell] == 0);
    saved_ticdata = ticdata;
    ticdata[BACKUPTICS - 1].cmds[NET_MAXPLAYERS - 1].buttons = 19;
    D_RegisterLoopCallbacks(&callbacks);
    assert(ticdata == saved_ticdata);
    assert(ticdata[BACKUPTICS - 1].cmds[NET_MAXPLAYERS - 1].buttons == 19);
    D_StartGameLoop();
    for (tick = 1; tick <= 66000; ++tick)
    {
        TryRunTics();
        assert(logical_tick == tick && grants == tick);
        assert(gametic == tick && maketic == tick && builds == tick && runs == tick);
        assert(I_GetTime() == tick && I_GetTime() == tick);
        assert(grants == tick);
    }
    assert(reads > grants);
    free(ticdata);
    ticdata = NULL;
    puts("clock widths: 66000 exact grants, commands and simulation ticks; reads stay pure");
    return 0;
}
