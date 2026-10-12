/* Deterministic initial profile: no host config files or save directory. */
#include "m_config.h"
#include "m_controls.h"
char *configdir = "";
void M_LoadDefaults(void) { key_up = 'w'; key_down = 's'; key_strafeleft = 'a'; key_straferight = 'd'; }
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
