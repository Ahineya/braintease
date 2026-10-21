#ifndef RIPLIX_WIFI_RESTART_H
#define RIPLIX_WIFI_RESTART_H
#include <stdint.h>
#include "wifi-control.h"
typedef struct {unsigned present,active,attempted,quiesced;} WifiRestartState;
typedef struct {
 void *opaque;
 void (*read)(void *,WifiRestartState *);
 void (*stop)(void *);
 /* Startup must create fresh nonce/keys. Called only after confirmed stop. */
 const char *(*start)(void *);
} WifiRestartOps;
typedef struct {unsigned busy;uint32_t attempts,generation;const char *error;} WifiRestart;
typedef struct {unsigned state,outcome;uint32_t attempts,generation;const char *error;} WifiRestartReply;
void wifi_restart(WifiRestart *,const WifiRestartOps *,unsigned,WifiRestartReply *);
void wifi_control(unsigned,WifiRestartReply *);
#endif
