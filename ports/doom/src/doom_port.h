#ifndef DOOM_PORT_H
#define DOOM_PORT_H
#include <stdint.h>
void Doom_NextTick(void);
uint32_t Doom_Tick(void);
void Doom_PostKey(unsigned int usage, int down);
void Doom_ConfigureCapture(const char *path, unsigned long frame, const char *input);
#endif
