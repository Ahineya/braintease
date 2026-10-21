/* C16 adaptation of SDL endian stream operations. Counts are logical octets. */
static Uint64 read_octets(SDL_RWops *src, int count, int little)
{
    Uint8 bytes[8]; Uint64 value = 0; int i;
    if (SDL_RWread(src, bytes, 1, count) != count) return 0;
    for (i = 0; i < count; i++) {
        int at = little ? count - 1 - i : i;
        value = (value << 8) | (bytes[at] & 255U);
    }
    return value;
}
static int write_octets(SDL_RWops *dst, Uint64 value, int count, int little)
{
    Uint8 bytes[8]; int i;
    for (i = 0; i < count; i++) {
        int at = little ? i : count - 1 - i;
        bytes[at] = value & 255U; value >>= 8;
    }
    return SDL_RWwrite(dst, bytes, count, 1);
}
Uint16 SDL_ReadLE16(SDL_RWops *s) { return read_octets(s,2,1); }
Uint16 SDL_ReadBE16(SDL_RWops *s) { return read_octets(s,2,0); }
Uint32 SDL_ReadLE32(SDL_RWops *s) { return read_octets(s,4,1); }
Uint32 SDL_ReadBE32(SDL_RWops *s) { return read_octets(s,4,0); }
Uint64 SDL_ReadLE64(SDL_RWops *s) { return read_octets(s,8,1); }
Uint64 SDL_ReadBE64(SDL_RWops *s) { return read_octets(s,8,0); }
Sint32 SDL_WriteLE16(SDL_RWops *s,Uint16 v) { return write_octets(s,v,2,1); }
Sint32 SDL_WriteBE16(SDL_RWops *s,Uint16 v) { return write_octets(s,v,2,0); }
Sint32 SDL_WriteLE32(SDL_RWops *s,Uint32 v) { return write_octets(s,v,4,1); }
Sint32 SDL_WriteBE32(SDL_RWops *s,Uint32 v) { return write_octets(s,v,4,0); }
Sint32 SDL_WriteLE64(SDL_RWops *s,Uint64 v) { return write_octets(s,v,8,1); }
Sint32 SDL_WriteBE64(SDL_RWops *s,Uint64 v) { return write_octets(s,v,8,0); }
