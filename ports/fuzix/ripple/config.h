/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef RIPPLE_FUZIX_CONFIG_H
#define RIPPLE_FUZIX_CONFIG_H
#include "mmu-profile.h"
#include "config-common.h"
#define PTABSIZE 32
#define UFTSIZE 32
#define OFTSIZE 128
#define ITABSIZE 128
#define CONFIG_PLATFORM_BRK
#define PROGTOP (0x001f0000UL + 64000UL)
#endif
