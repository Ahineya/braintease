/* Exercise the real responder and ticcmd builder with actual port defaults.
 * A native/Ripple pixel comparison alone misses keys ignored by both builds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Keep fixture device mocks distinct from the linked runtime's real ABI. */
#define vm_input_read fixture_input_read
#define vm_input_state fixture_input_state
#define vm_key_is_down fixture_key_is_down
#include "controls_defaults.inc"
#include "input_commands.inc"
#include "platform_keyboard.inc"

int ticdup = 1;
int mouseSensitivity = 5;
static int commands;
static event_t queued_event;
static int event_pending;
static int posted_events;

void D_PostEvent(event_t *event)
{
    if (event_pending) {
        puts("INPUT FAIL adapter did not drain event queue");
        exit(1);
    }
    queued_event = *event;
    event_pending = 1;
    ++posted_events;
}

void D_ProcessEvents(void)
{
    if (event_pending) {
        event_pending = 0;
        G_Responder(&queued_event);
    }
}

char HU_dequeueChatChar(void) { return 0; }
boolean HU_Responder(event_t *event) { (void)event; return false; }
boolean ST_Responder(event_t *event) { (void)event; return false; }
boolean AM_Responder(event_t *event) { (void)event; return false; }
boolean F_Responder(event_t *event) { (void)event; return false; }
void M_StartControlPanel(void) {}

static int G_NextWeapon(int direction)
{
    (void)direction;
    puts("INPUT FAIL unexpected weapon cycling");
    exit(1);
    return 0;
}

static VmInputEvent pending_input[24];
static unsigned short pending_count, pending_position, input_overflow;
static VmInputState recovery_state;

unsigned short vm_input_read(VmInputEvent *events, unsigned short capacity, VmInputBatch *batch)
{
    unsigned short count = pending_count - pending_position;
    if (count > capacity) count = capacity;
    memcpy(events, pending_input + pending_position, (size_t)count * sizeof(*events));
    pending_position += count;
    batch->count = count;
    batch->overflow = input_overflow;
    return VM_DEVICE_OK;
}
unsigned short vm_input_state(VmInputState *state)
{
    *state = recovery_state;
    input_overflow = 0;
    return VM_DEVICE_OK;
}
int vm_key_is_down(const VmInputState *state, unsigned short usage)
{
    return (state->held[usage >> 4] & (1U << (usage & 15))) != 0;
}
void I_Error(char *error, ...)
{
    puts(error);
    exit(1);
}

static void reset(void)
{
    memset(gamekeydown, 0, sizeof(gamekeydown));
    memset(held, 0, sizeof(held));
    memset(mousearray, 0, sizeof(mousearray));
    memset(joyarray, 0, sizeof(joyarray));
    turnheld = next_weapon = 0;
    mousex = mousey = joyxmove = joyymove = joystrafemove = 0;
    dclicktime = dclickstate = dclicks = 0;
    dclicktime2 = dclickstate2 = dclicks2 = 0;
    sendpause = sendsave = demoplayback = singledemo = false;
    lowres_turn = testcontrols = false;
    gamestate = GS_LEVEL;
    gameaction = ga_nothing;
    consoleplayer = 0;
    playeringame[0] = true;
    event_pending = posted_events = 0;
}

static void key(unsigned int usage, int down)
{
    Doom_PostKey(usage, down);
}

static void expect(const char *name, int forward, int side, int turn, int buttons)
{
    ticcmd_t cmd;
    memset(&cmd, 99, sizeof(cmd));
    G_BuildTiccmd(&cmd, 0);
    ++commands;
    if (cmd.forwardmove != forward || cmd.sidemove != side
        || cmd.angleturn != turn || cmd.buttons != buttons) {
        printf("INPUT FAIL %s: %d %d %d %d\n", name, (int)cmd.forwardmove,
               (int)cmd.sidemove, (int)cmd.angleturn, (int)cmd.buttons);
        exit(1);
    }
}

static void movement_aliases(void)
{
    reset();
    expect("idle", 0, 0, 0, 0);
    key(82, 1);
    expect("up arrow", 25, 0, 0, 0);
    key(26, 1);
    expect("up plus W does not double", 25, 0, 0, 0);
    key(82, 0);
    expect("release arrow keeps W", 25, 0, 0, 0);
    key(26, 0);
    expect("release W", 0, 0, 0, 0);
    key(81, 1);
    expect("down arrow", -25, 0, 0, 0);
    key(22, 1);
    expect("down plus S does not double", -25, 0, 0, 0);
    key(81, 0);
    expect("release arrow keeps S", -25, 0, 0, 0);
    key(26, 1);
    expect("W and S cancel", 0, 0, 0, 0);
    key(22, 0);
    expect("release S keeps W", 25, 0, 0, 0);
    key(26, 0);
    expect("release movement", 0, 0, 0, 0);
}

static void action_buttons(void)
{
    reset();
    key(224, 1);
    expect("Control fires", 0, 0, 0, BT_ATTACK);
    expect("Control held fires", 0, 0, 0, BT_ATTACK);
    key(44, 1);
    expect("Space and Control", 0, 0, 0, BT_ATTACK | BT_USE);
    key(224, 0);
    expect("release Control keeps Space", 0, 0, 0, BT_USE);
    key(44, 0);
    expect("release Space", 0, 0, 0, 0);
    key(26, 1);
    key(224, 1);
    key(44, 1);
    expect("move fire and use", 25, 0, 0, BT_ATTACK | BT_USE);
    key(26, 0);
    key(224, 0);
    key(44, 0);
    expect("release combination", 0, 0, 0, 0);
}

static void strafe_aliases(void)
{
    reset();
    key(4, 1);
    expect("A strafes left", 0, -24, 0, 0);
    key(54, 1);
    expect("A plus comma does not double", 0, -24, 0, 0);
    key(4, 0);
    expect("comma remains held", 0, -24, 0, 0);
    key(54, 0);
    key(7, 1);
    expect("D strafes right", 0, 24, 0, 0);
    key(55, 1);
    expect("D plus period does not double", 0, 24, 0, 0);
    key(7, 0);
    expect("period remains held", 0, 24, 0, 0);
    key(54, 1);
    expect("strafe directions cancel", 0, 0, 0, 0);
    key(55, 0);
    key(54, 0);
    expect("release strafe", 0, 0, 0, 0);
}

static void modifiers_and_turning(void)
{
    int i;
    reset();
    key(26, 1);
    key(7, 1);
    expect("walk diagonal", 25, 24, 0, 0);
    key(225, 1);
    expect("Shift runs diagonal", 50, 40, 0, 0);
    key(225, 0);
    expect("release Shift walks", 25, 24, 0, 0);
    key(26, 0);
    key(7, 0);
    key(79, 1);
    expect("right arrow turns slowly", 0, 0, -320, 0);
    key(226, 1);
    expect("Alt right strafes", 0, 24, 0, 0);
    key(225, 1);
    expect("Shift Alt right runs strafe", 0, 40, 0, 0);
    key(79, 0);
    key(80, 1);
    expect("Shift Alt left runs strafe", 0, -40, 0, 0);
    key(225, 0);
    expect("Alt left walks strafe", 0, -24, 0, 0);
    key(226, 0);
    expect("release Alt turns", 0, 0, 640, 0);
    key(80, 0);
    expect("release turn", 0, 0, 0, 0);
    key(80, 1);
    for (i = 0; i < 5; ++i)
        expect("fresh turn starts slowly", 0, 0, 320, 0);
    expect("held turn accelerates", 0, 0, 640, 0);
    key(225, 1);
    expect("Shift accelerates turn", 0, 0, 1280, 0);
    key(80, 0);
    key(225, 0);
    expect("release modifiers", 0, 0, 0, 0);
}

static void expect_posts(int count)
{
    if (posted_events != count) {
        printf("INPUT FAIL duplicate/merged modifier events %d\n", posted_events);
        exit(1);
    }
}

static void paired_physical_modifiers(void)
{
    reset();
    key(224, 1); /* Left Control. */
    expect("left Control", 0, 0, 0, BT_ATTACK);
    key(224, 1);
    expect("duplicate Control down", 0, 0, 0, BT_ATTACK);
    expect_posts(1);
    key(228, 1); /* Right Control has the same logical Doom key. */
    expect("both Controls", 0, 0, 0, BT_ATTACK);
    key(224, 0);
    expect("release left keeps right Control", 0, 0, 0, BT_ATTACK);
    expect_posts(1);
    key(228, 0);
    expect("release final Control", 0, 0, 0, 0);
    expect_posts(2);
    key(228, 1);
    expect("right Control", 0, 0, 0, BT_ATTACK);
    key(224, 1);
    expect("both Controls reverse order", 0, 0, 0, BT_ATTACK);
    key(228, 0);
    expect("release right keeps left Control", 0, 0, 0, BT_ATTACK);
    key(224, 0);
    expect("release reverse Controls", 0, 0, 0, 0);
    expect_posts(4);

    reset();
    key(26, 1);
    key(225, 1);
    key(229, 1);
    expect("both Shifts run", 50, 0, 0, 0);
    key(225, 0);
    expect("release left keeps right Shift", 50, 0, 0, 0);
    key(229, 0);
    expect("release final Shift walks", 25, 0, 0, 0);

    reset();
    key(79, 1);
    key(226, 1);
    key(230, 1);
    expect("both Alts strafe", 0, 24, 0, 0);
    key(226, 0);
    expect("release left keeps right Alt", 0, 24, 0, 0);
    key(230, 0);
    expect("release final Alt turns", 0, 0, -320, 0);
    key(79, 0);
    expect("release right arrow", 0, 0, 0, 0);
    key(256, 1);
    key(0, 1);
    expect("unmapped usages ignored", 0, 0, 0, 0);
    expect_posts(4);
}

static void disabled_mouse(void)
{
    if (usemouse != DOOM_MOUSE) {
        puts("INPUT FAIL mouse build default"); exit(1);
    }
    reset();
    usemouse = 0;
    Doom_PostMouse(7U, 100L, -100L);
    expect("disabled mouse ignores motion and buttons", 0, 0, 0, 0);
    expect_posts(0);
    memset(pending_input, 0, sizeof(pending_input));
    pending_input[0].type = VM_INPUT_MOTION;
    pending_input[0].flags = VM_INPUT_RELATIVE;
    pending_input[0].dx = 100L;
    pending_input[1].type = VM_INPUT_BUTTON;
    pending_input[1].buttons = 1;
    pending_input[2].type = VM_INPUT_KEY;
    pending_input[2].code = 26; /* W still moves forward. */
    pending_input[2].flags = VM_INPUT_DOWN;
    pending_count = 3;
    pending_position = input_overflow = 0;
    I_StartTic();
    expect("disabled device mouse retains keyboard", 25, 0, 0, 0);
    memset(&recovery_state, 0, sizeof(recovery_state));
    recovery_state.buttons = 7;
    recovery_state.held[26 >> 4] = 1U << (26 & 15);
    pending_count = pending_position = 0;
    input_overflow = 1;
    I_StartTic();
    expect("disabled mouse ignores recovered buttons", 25, 0, 0, 0);
    expect_posts(1);
}

static void mouse_controls(void)
{
    reset();
    usemouse = 1;
    Doom_PostMouse(1U, 10L, -3L);
    expect("mouse fire, forward and turn", 3, 0, -80, BT_ATTACK);
    Doom_PostMouse(0U, 0L, 0L);
    expect("mouse release", 0, 0, 0, 0);
    key(226,1);
    Doom_PostMouse(0U, 5L, 0L);
    expect("Alt mouse strafe", 0, 10, 0, 0);
    key(226,0);
    Doom_PostMouse(0U, 0L, 0L);
    expect("mouse movement consumed", 0, 0, 0, 0);
    Doom_PostMouse(0U, 999999L, -999999L);
    if (queued_event.data2 != 1024 || queued_event.data3 != 1024) {
        puts("INPUT FAIL mouse deltas not bounded"); exit(1);
    }
    Doom_PostMouse(0U, 0L, 0L);
    expect("mouse recovery clears movement", 0, 0, 0, 0);
    {
        long dx = Doom_AddMouseDelta(0L, 7L);
        dx = Doom_AddMouseDelta(dx, 5L);
        /* A click between motions must not erase the tick's accumulated delta. */
        Doom_PostMouse(1U, 0L, 0L);
        Doom_PostMouse(1U, dx, 0L);
        expect("mouse batch retains all movement and buttons", 0, 0, -96, BT_ATTACK);
        if (Doom_AddMouseDelta(1020L, 2147483647L) != 1024L
            || Doom_AddMouseDelta(-1020L, -2147483647L - 1L) != -1024L) {
            puts("INPUT FAIL mouse accumulation overflow"); exit(1);
        }
    }
}

static void device_mouse_batches(void)
{
    unsigned short i;
    reset();
    memset(pending_input,0,sizeof(pending_input));
    for (i=0;i<21;++i) {
        pending_input[i].type = VM_INPUT_MOTION;
        pending_input[i].flags = VM_INPUT_RELATIVE;
        pending_input[i].dx = 1L;
        pending_input[i].buttons = i < 10 ? 0 : 1;
    }
    pending_input[10].type = VM_INPUT_BUTTON;
    pending_input[10].flags = VM_INPUT_DOWN;
    pending_input[10].dx = 0L;
    pending_count = 21;
    pending_position = input_overflow = 0;
    I_StartTic();
    expect("mouse movement spans batches and a click", 0, 0, -160, BT_ATTACK);
    /* Recovery replaces held controls and discards this tick's stale motion. */
    memset(&recovery_state,0,sizeof(recovery_state));
    pending_count = pending_position = 0;
    input_overflow = 1;
    I_StartTic();
    expect("mouse overflow recovery releases buttons", 0, 0, 0, 0);
}

int main(void)
{
    M_LoadDefaults();
    action_buttons();
    movement_aliases();
    strafe_aliases();
    modifiers_and_turning();
    paired_physical_modifiers();
    disabled_mouse();
    mouse_controls();
    device_mouse_batches();
    if (commands != 72) {
        printf("INPUT FAIL command count %d\n", commands);
        return 1;
    }
    puts("INPUT OK: arrows/WASD, Control fire, Space use, Shift/Alt, aliases, releases and mouse");
    return 0;
}
