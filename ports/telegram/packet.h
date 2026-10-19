/* SPDX-License-Identifier: MIT */
#ifndef TG_PACKET_H
#define TG_PACKET_H
#include "core.h"
int tg_encrypt(const TgByte *,const TgByte *,const TgByte *,TgU64,TgU32,const TgByte *,unsigned,TgByte *,unsigned,unsigned);
/* Validates authentication before yielding any header or body. Decrypts in place.
 * Returns body length, or -1. Does not enforce session/replay policy. */
int tg_decrypt(const TgByte *,TgByte *,unsigned,unsigned);
#endif
