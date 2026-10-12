/* Deterministic initial profile: no host config files or save directory. */
#include "m_config.h"
#include "m_controls.h"
#include "doomkeys.h"
char *configdir = "";
void M_LoadDefaults(void)
{
    key_up = KEY_UPARROW;
    key_down = KEY_DOWNARROW;
    key_left = KEY_LEFTARROW;
    key_right = KEY_RIGHTARROW;
    key_strafeleft = 'a';
    key_straferight = 'd';
    /* Doomgeneric's synthetic KEY_FIRE/KEY_USE are not physical key codes. */
    key_fire = KEY_RCTRL;
    key_use = ' ';
    key_strafe = KEY_RALT;
    key_speed = KEY_RSHIFT;
}
void M_SaveDefaults(void) {}
void M_SaveDefaultsAlternate(char *main, char *extra) {}
void M_SetConfigDir(char *dir) {}
void M_SetConfigFilenames(char *main, char *extra) {}
void M_BindVariable(char *name, void *variable) {}
boolean M_SetVariable(char *name, char *value) { return false; }
int M_GetIntVariable(char *name) { return 0; }
const char *M_GetStrVariable(char *name) { return ""; }
float M_GetFloatVariable(char *name) { return 0.0f; }
char *M_GetSaveGameDir(char *iwadname) { return ""; }
