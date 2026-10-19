/* SPDX-License-Identifier: MIT */
#ifndef TG_TL_H
#define TG_TL_H
#include "core.h"
#include "tl_fields.h"
typedef struct {unsigned op,condition,key;} TgRule;
typedef struct {TgU32 id;unsigned start,count;} TgConstructor;
typedef struct {TgByte *p;unsigned n;} TgSlice;
typedef struct {TgU32 id,flags[3];TgSlice fields[TG_FIELDS];} TgView;
extern const TgRule tg_rules[];
extern const TgConstructor tg_constructors[];
extern const unsigned tg_constructor_count;
int tg_view(TgTl *,TgView *);
int tg_slice_view(TgSlice,TgView *);
TgU32 tg_v32(const TgView *,unsigned);
TgU64 tg_v64(const TgView *,unsigned);
TgSlice tg_vtext(const TgView *,unsigned);
int tg_vector(TgSlice,TgTl *,unsigned *);
TgU32 tg_tl_error(void);
#endif
