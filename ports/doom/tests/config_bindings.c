/* Compile the actual startup binding routine and real controls/joystick code.
 * The mock registry checks names and destinations before stack buffers expire. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "config.h"
#include "doomdef.h"
#include "dstrings.h"
#include "m_config.h"
#include "m_controls.h"
#include "i_joystick.h"

int mouseSensitivity, sfxVolume, musicVolume, showMessages;
int screenblocks, detailLevel, snd_channels, vanilla_savegame_limit;
int vanilla_demo_limit, show_endoom;
char *chat_macros[10];
static unsigned int video_calls, sound_calls;
void I_BindVideoVariables(void) { ++video_calls; }
void I_BindSoundVariables(void) { ++sound_calls; }

#include "config_bindings.inc"

typedef struct {
    const char *name;
    void *target;
} Expected;
static Expected fixed[] = {
    {"use_joystick", NULL},
    {"joystick_index", NULL},
    {"joystick_x_axis", NULL},
    {"joystick_y_axis", NULL},
    {"joystick_strafe_axis", NULL},
    {"joystick_x_invert", NULL},
    {"joystick_y_invert", NULL},
    {"joystick_strafe_invert", NULL},
    {"key_right", &key_right},
    {"key_left", &key_left},
    {"key_up", &key_up},
    {"key_down", &key_down},
    {"key_strafeleft", &key_strafeleft},
    {"key_straferight", &key_straferight},
    {"key_fire", &key_fire},
    {"key_use", &key_use},
    {"key_strafe", &key_strafe},
    {"key_speed", &key_speed},
    {"mouseb_fire", &mousebfire},
    {"mouseb_strafe", &mousebstrafe},
    {"mouseb_forward", &mousebforward},
    {"joyb_fire", &joybfire},
    {"joyb_strafe", &joybstrafe},
    {"joyb_use", &joybuse},
    {"joyb_speed", &joybspeed},
    {"joyb_menu_activate", &joybmenu},
    {"joyb_strafeleft", &joybstrafeleft},
    {"joyb_straferight", &joybstraferight},
    {"mouseb_strafeleft", &mousebstrafeleft},
    {"mouseb_straferight", &mousebstraferight},
    {"mouseb_use", &mousebuse},
    {"mouseb_backward", &mousebbackward},
    {"dclick_use", &dclick_use},
    {"key_pause", &key_pause},
    {"key_message_refresh", &key_message_refresh},
    {"key_weapon1", &key_weapon1},
    {"key_weapon2", &key_weapon2},
    {"key_weapon3", &key_weapon3},
    {"key_weapon4", &key_weapon4},
    {"key_weapon5", &key_weapon5},
    {"key_weapon6", &key_weapon6},
    {"key_weapon7", &key_weapon7},
    {"key_weapon8", &key_weapon8},
    {"key_prevweapon", &key_prevweapon},
    {"key_nextweapon", &key_nextweapon},
    {"joyb_prevweapon", &joybprevweapon},
    {"joyb_nextweapon", &joybnextweapon},
    {"mouseb_prevweapon", &mousebprevweapon},
    {"mouseb_nextweapon", &mousebnextweapon},
    {"key_map_north", &key_map_north},
    {"key_map_south", &key_map_south},
    {"key_map_east", &key_map_east},
    {"key_map_west", &key_map_west},
    {"key_map_zoomin", &key_map_zoomin},
    {"key_map_zoomout", &key_map_zoomout},
    {"key_map_toggle", &key_map_toggle},
    {"key_map_maxzoom", &key_map_maxzoom},
    {"key_map_follow", &key_map_follow},
    {"key_map_grid", &key_map_grid},
    {"key_map_mark", &key_map_mark},
    {"key_map_clearmark", &key_map_clearmark},
    {"key_menu_activate", &key_menu_activate},
    {"key_menu_up", &key_menu_up},
    {"key_menu_down", &key_menu_down},
    {"key_menu_left", &key_menu_left},
    {"key_menu_right", &key_menu_right},
    {"key_menu_back", &key_menu_back},
    {"key_menu_forward", &key_menu_forward},
    {"key_menu_confirm", &key_menu_confirm},
    {"key_menu_abort", &key_menu_abort},
    {"key_menu_help", &key_menu_help},
    {"key_menu_save", &key_menu_save},
    {"key_menu_load", &key_menu_load},
    {"key_menu_volume", &key_menu_volume},
    {"key_menu_detail", &key_menu_detail},
    {"key_menu_qsave", &key_menu_qsave},
    {"key_menu_endgame", &key_menu_endgame},
    {"key_menu_messages", &key_menu_messages},
    {"key_menu_qload", &key_menu_qload},
    {"key_menu_quit", &key_menu_quit},
    {"key_menu_gamma", &key_menu_gamma},
    {"key_menu_incscreen", &key_menu_incscreen},
    {"key_menu_decscreen", &key_menu_decscreen},
    {"key_menu_screenshot", &key_menu_screenshot},
    {"key_demo_quit", &key_demo_quit},
    {"key_spy", &key_spy},
    {"key_multi_msg", &key_multi_msg},
    {"mouse_sensitivity", &mouseSensitivity},
    {"sfx_volume", &sfxVolume},
    {"music_volume", &musicVolume},
    {"show_messages", &showMessages},
    {"screenblocks", &screenblocks},
    {"detaillevel", &detailLevel},
    {"snd_channels", &snd_channels},
    {"vanilla_savegame_limit", &vanilla_savegame_limit},
    {"vanilla_demo_limit", &vanilla_demo_limit},
    {"show_endoom", &show_endoom},
};
static unsigned int seen[97];
static unsigned int chat_seen[10], player_seen[8], joystick_seen[10];
static unsigned int total, chat_count, player_count, joystick_count;
static int *joystick_base;

static void check(int condition, const char *message)
{
    if (!condition) {
        printf("CONFIG BINDINGS ERROR %s\n", message);
        exit(1);
    }
}

void M_BindVariable(char *name, void *variable)
{
    unsigned int i;
    ++total;
    check(variable != NULL, "null binding target");
    if (strncmp(name, "chatmacro", 9) == 0) {
        check(name[9] >= '0' && name[9] <= '9' && name[10] == 0,
              "chat macro suffix/terminator");
        i = (unsigned int)(name[9] - '0');
        check(variable == &chat_macros[i] && !chat_seen[i], "chat macro target/duplicate");
        chat_seen[i] = 1;
        ++chat_count;
        return;
    }
    if (strncmp(name, "key_multi_msgplayer", 19) == 0) {
        check(name[19] >= '1' && name[19] <= '8' && name[20] == 0,
              "player suffix/terminator");
        i = (unsigned int)(name[19] - '1');
        check(variable == &key_multi_msgplayer[i] && !player_seen[i], "player target/duplicate");
        player_seen[i] = 1;
        ++player_count;
        return;
    }
    if (strncmp(name, "joystick_physical_button", 24) == 0) {
        check(name[24] >= '0' && name[24] <= '9' && name[25] == 0,
              "joystick suffix/terminator");
        i = (unsigned int)(name[24] - '0');
        if (i == 0) joystick_base = variable;
        check(joystick_base != NULL && variable == &joystick_base[i]
              && *(int *)variable == (int)i && !joystick_seen[i], "joystick target/default/duplicate");
        joystick_seen[i] = 1;
        ++joystick_count;
        return;
    }
    for (i = 0; i < sizeof(fixed) / sizeof(fixed[0]); ++i) {
        if (strcmp(name, fixed[i].name) == 0) {
            check(!seen[i], "duplicate fixed name");
            check(fixed[i].target == NULL || variable == fixed[i].target, "fixed binding target");
            seen[i] = 1;
            return;
        }
    }
    check(0, "unexpected binding name");
}

static void reset_seen(void)
{
    memset(seen, 0, sizeof(seen));
    memset(chat_seen, 0, sizeof(chat_seen));
    memset(player_seen, 0, sizeof(player_seen));
    memset(joystick_seen, 0, sizeof(joystick_seen));
    total = chat_count = player_count = joystick_count = 0;
    joystick_base = NULL;
}

int main(void)
{
    unsigned int i;
    check(sizeof(fixed) / sizeof(fixed[0]) == 97, "fixed registry size");
    for (i = 0; i < 8; ++i) key_multi_msgplayer[i] = 42;
    reset_seen();
    D_BindVariables();
    check(video_calls == 1 && sound_calls == 1, "platform registration callbacks");
    check(total == 121 && chat_count == 10 && player_count == 4 && joystick_count == 10,
          "startup registration counts");
    for (i = 0; i < 97; ++i) check(seen[i], "missing fixed binding");
    check(key_multi_msgplayer[0] == HUSTR_KEYGREEN &&
          key_multi_msgplayer[1] == HUSTR_KEYINDIGO &&
          key_multi_msgplayer[2] == HUSTR_KEYBROWN &&
          key_multi_msgplayer[3] == HUSTR_KEYRED, "default player keys");
    for (i = 4; i < 8; ++i) check(key_multi_msgplayer[i] == 42, "unbound player defaults");
    reset_seen();
    M_BindChatControls(8);
    check(total == 9 && player_count == 8, "all eight player names");
    reset_seen();
    M_BindChatControls(0);
    check(total == 1 && player_count == 0, "empty player list");
    puts("CONFIG BINDINGS OK 24 numbered names, 97 fixed names, targets and player defaults");
    return 0;
}
