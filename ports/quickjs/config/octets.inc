/* SPDX-License-Identifier: MIT
 * Logical little-endian bytecode octets, independent of C object layout.
 * Pointer arithmetic goes through the compiler's bank-aware operations.
 */
static inline uint64_t get_u64(const qjs_octet_t *p)
{
    uint64_t value = 0;
    int i;
    for (i = 0; i < 8; ++i) value |= (uint64_t)(p[i] & 255U) << (i * 8);
    return value;
}
static inline int64_t get_i64(const qjs_octet_t *p) { return (int64_t)get_u64(p); }
static inline void put_u64(qjs_octet_t *p, uint64_t value)
{
    int i;
    for (i = 0; i < 8; ++i) { p[i] = (qjs_octet_t)(value & 255U); value >>= 8; }
}
static inline uint32_t get_u32(const qjs_octet_t *p)
{
    return (uint32_t)(p[0] & 255U) | ((uint32_t)(p[1] & 255U) << 8) |
           ((uint32_t)(p[2] & 255U) << 16) | ((uint32_t)(p[3] & 255U) << 24);
}
static inline int32_t get_i32(const qjs_octet_t *p) { return (int32_t)get_u32(p); }
static inline void put_u32(qjs_octet_t *p, uint32_t value)
{
    int i;
    for (i = 0; i < 4; ++i) { p[i] = (qjs_octet_t)(value & 255U); value >>= 8; }
}
static inline uint32_t get_u16(const qjs_octet_t *p)
{
    return (uint32_t)(p[0] & 255U) | ((uint32_t)(p[1] & 255U) << 8);
}
static inline int32_t get_i16(const qjs_octet_t *p)
{
    uint32_t value = get_u16(p);
    return value >= 32768UL ? (int32_t)value - 65536L : (int32_t)value;
}
static inline void put_u16(qjs_octet_t *p, uint16_t value)
{
    p[0] = value & 255U; p[1] = (value >> 8) & 255U;
}
static inline uint32_t get_u8(const qjs_octet_t *p) { return *p & 255U; }
static inline int32_t get_i8(const qjs_octet_t *p)
{
    int value = *p & 255U;
    return value >= 128 ? value - 256 : value;
}
static inline void put_u8(qjs_octet_t *p, qjs_octet_t value) { *p = value & 255U; }
