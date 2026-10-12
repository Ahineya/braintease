/* Exercise the real responder and ticcmd builder with actual port defaults.
 * A native/Ripple pixel comparison alone misses keys ignored by both builds. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

int main(void)
{
    M_LoadDefaults();
    action_buttons();
    movement_aliases();
    strafe_aliases();
    modifiers_and_turning();
    paired_physical_modifiers();
    if (commands != 61) {
        printf("INPUT FAIL command count %d\n", commands);
        return 1;
    }
    puts("INPUT OK: arrows/WASD, Control fire, Space use, Shift/Alt, aliases and releases");
    return 0;
}
