"""Checked storage adaptations, before private C integer widening.

Strings contain native Latin1/UTF-16 units; bytecode contains logical octets.
These representations must never share a size calculation or a memcpy cast.
The string changes also run against the complete host engine in tests.
"""
from adapt import replace, function_body
from core_frames import transform_function


def string_storage(source):
    source = replace(source, '#define JS_STRING_LEN_MAX ((1 << 30) - 1)',
                     '#define JS_STRING_LEN_MAX ((1UL << 30) - 1)')
    source = replace(source, '/* Note: the string contents are uninitialized */', '''/* Native string units, independent of serialized octet widths. */
static inline size_t js_string_char_size(int wide)
{
    return wide ? sizeof(uint16_t) : sizeof(uint8_t);
}
static inline size_t js_string_data_size(size_t len, int wide)
{
    return len * js_string_char_size(wide) + (wide ? 0 : 1);
}

/* Note: the string contents are uninitialized */''')
    for before, after, count in [
        ('(max_len << is_wide_char) + 1 - is_wide_char',
         'js_string_data_size(max_len, is_wide_char)', 1),
        ('(str->len << str->is_wide_char) +\n                             1 - str->is_wide_char',
         'js_string_data_size(str->len, str->is_wide_char)', 1),
        ('(str->len << str->is_wide_char) +\n                   1 - str->is_wide_char',
         'js_string_data_size(str->len, str->is_wide_char)', 1),
        ('(str->len << str->is_wide_char) +\n                          1 - str->is_wide_char',
         'js_string_data_size(str->len, str->is_wide_char)', 1),
        ('(p->len << p->is_wide_char) +\n                             1 - p->is_wide_char',
         'js_string_data_size(p->len, p->is_wide_char)', 1),
        ('(s->len << s->is_wide_char) + 1 - s->is_wide_char',
         'js_string_data_size(s->len, s->is_wide_char)', 1),
        ('(new_size << s->is_wide_char) + 1 - s->is_wide_char',
         'js_string_data_size(new_size, s->is_wide_char)', 1),
        ('sizeof(JSString) + (size << 1)',
         'sizeof(JSString) + (size_t)size * sizeof(uint16_t)', 1),
        ('size += slack >> 1;', 'size += slack / sizeof(uint16_t);', 1),
        ('(slack >> s->is_wide_char)', '(slack / js_string_char_size(s->is_wide_char))', 1),
        ('memcpy(str->u.str16, buf, len * 2);',
         'memcpy(str->u.str16, buf, (size_t)len * sizeof(*buf));', 1),
        ('memcpy(&s->str->u.str16[s->len], p, len << 1);',
         'memcpy(&s->str->u.str16[s->len], p, (size_t)len * sizeof(*p));', 1),
        ('memcpy(dst, p->u.str16 + offset, len * 2);',
         'memcpy(dst, p->u.str16 + offset, (size_t)len * sizeof(*dst));', 1),
        ('((p1->len + p2->len) << 1)',
         '((size_t)(p1->len + p2->len) * sizeof(uint16_t))', 1),
        ('memcpy(p1->u.str16 + p1->len, p2->u.str16, p2->len << 1);',
         'memcpy(p1->u.str16 + p1->len, p2->u.str16, (size_t)p2->len * sizeof(uint16_t));', 1),
        # Both uint16_t operands promote to unsigned C16 int. Cast BEFORE
        # subtraction or a negative comparison wraps to a positive value.
        ('c = src1[i] - src2[i];', 'c = (int32_t)src1[i] - (int32_t)src2[i];', 2),
    ]:
        source = replace(source, before, after, count)
    # The signed private length is not a licence to allocate a wrapped size.
    source = replace(source, '    str = js_malloc_rt(rt, sizeof(JSString) + js_string_data_size(max_len, is_wide_char));',
                     '''    if (max_len < 0 || (size_t)max_len > JS_STRING_LEN_MAX)
        return NULL;
    str = js_malloc_rt(rt, sizeof(JSString) + js_string_data_size(max_len, is_wide_char));''')
    source = replace(source, '    p_start = (const uint8_t *)buf;', '''    if (buf_len > JS_STRING_LEN_MAX)
        return JS_ThrowInternalError(ctx, "string too long");
    p_start = (const uint8_t *)buf;''')
    # Upstream retains val before export, then leaks that reference if the
    # UTF8 allocation fails. JS_EXCEPTION is also safe to free when ToString
    # itself failed. Keep the repair in generated sources only.
    source = transform_function(source, 'JS_ToCStringLen2', lambda body: replace(
        body, ' fail:\n    if (plen)', ' fail:\n    JS_FreeValue(ctx, val);\n    if (plen)'))
    # Regexp captures point into native string storage. Keep the wide flag
    # itself unchanged: libregexp still needs it to distinguish Latin1/UTF16.
    for expression, count in [
        ('(captures[1] - captures[0])', 1),
        ('(captures[2 * k] - sp->u.str8)', 1),
        ('(captures[2 * k + 1] - sp->u.str8)', 1),
        ('(capture[1] - str_buf)', 2),
        ('(capture[0] - str_buf)', 2),
        ('(match[0] - str_buf)', 1),
        ('(match[1] - str_buf)', 1),
    ]:
        source = replace(source, expression + ' >> shift',
                         expression + ' / js_string_char_size(shift)', count)
    return source


def serialized_storage(source):
    # The adapted cutils accessors are explicitly little endian. Never swap
    # them according to native C object layout or copy a uint64_t object.
    for bits in (16, 32, 64):
        source = function_body(source, f'bc_put_u{bits}',
                               f'    dbuf_put_u{bits}(&s->dbuf, v);')
        source = replace(source, f'    if (is_be())\n        v = bswap{bits}(v);', '', 1)
    source = replace(source, '    if (is_be())\n        bc_byte_swap(bc_buf, bc_len);', '', 2)
    source = replace(source, '''    memcpy(p->u.str8, s->ptr, size);
    s->ptr += size;
    if (is_wide_char) {
        if (is_be()) {
            uint32_t i;
            for (i = 0; i < len; i++)
                p->u.str16[i] = bswap16(p->u.str16[i]);
        }
    } else {
        p->u.str8[size] = '\\0'; /* add the trailing zero for 8 bit strings */
    }''', '''    if (is_wide_char) {
        uint32_t i;
        for (i = 0; i < len; i++)
            p->u.str16[i] = get_u16(s->ptr + (size_t)i * 2);
    } else {
        memcpy(p->u.str8, s->ptr, len);
        p->u.str8[len] = '\\0';
    }
    s->ptr += size;''')
    return source
