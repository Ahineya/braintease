/* NetHack level operands contain packed OCTETS even on a C16 machine.
 * Explicitly sign-extend the small signed fields; char itself is 16 bits.
 * Cast before shifts so 16-bit int expressions do not lose upper fields.
 * Modified for Ripple/Fuse-X on 2026-10-19. See upstream/dat/license. */
#define NH_SIGNED_OCTET(v) ((long)((((unsigned long)(v) & 255UL) ^ 128UL)) - 128L)
#define SP_COORD_X(v) NH_SIGNED_OCTET(v)
#define SP_COORD_Y(v) NH_SIGNED_OCTET((unsigned long)(v) >> 16)
#define SP_COORD_PACK(x,y) ((long)(((unsigned long)(x) & 255UL) | (((unsigned long)(y) & 255UL) << 16)))
#define SP_COORD_PACK_RANDOM(f) (SP_COORD_IS_RANDOM | (f))
#define SP_REGION_X1(v) NH_SIGNED_OCTET(v)
#define SP_REGION_Y1(v) NH_SIGNED_OCTET((unsigned long)(v) >> 8)
#define SP_REGION_X2(v) NH_SIGNED_OCTET((unsigned long)(v) >> 16)
#define SP_REGION_Y2(v) NH_SIGNED_OCTET((unsigned long)(v) >> 24)
#define SP_REGION_PACK(x1,y1,x2,y2) ((long)(((unsigned long)(x1) & 255UL) | (((unsigned long)(y1) & 255UL) << 8) | (((unsigned long)(x2) & 255UL) << 16) | (((unsigned long)(y2) & 255UL) << 24)))
#define SP_MONST_CLASS(v) NH_SIGNED_OCTET(v)
#define SP_MONST_PM(v) ((long)(((unsigned long)(v) >> 8) & 65535UL) - 10L)
#define SP_MONST_PACK(pm,cls) (((10L + (long)(pm)) << 8) | ((long)(cls) & 255L))
#define SP_OBJ_CLASS(v) NH_SIGNED_OCTET(v)
#define SP_OBJ_TYP(v) ((long)(((unsigned long)(v) >> 8) & 65535UL) - 10L)
#define SP_OBJ_PACK(ob,cls) (((10L + (long)(ob)) << 8) | ((long)(cls) & 255L))
#define SP_MAPCHAR_TYP(v) NH_SIGNED_OCTET(v)
#define SP_MAPCHAR_LIT(v) ((long)(((unsigned long)(v) >> 8) & 65535UL) - 10L)
#define SP_MAPCHAR_PACK(typ,lit) (((10L + (long)(lit)) << 8) | ((long)(typ) & 255L))
