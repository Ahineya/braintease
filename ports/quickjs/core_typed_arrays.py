"""ArrayBuffer octets are logical bytes, never native C scalar objects.

Keep upstream bounds/conversion/detachment checks. Replace the representation
dependent fast paths with explicit little-endian loads and stores. Internal
sort indexes remain native uint32_t objects; array elements do not.
"""
from adapt import replace
from core_frames import transform_function


def typed_array_storage(source):
    helpers = '''/* Logical ArrayBuffer elements, independent of CHAR_BIT. */
static int32_t qjs_ta_i8(const uint8_t *p) {
    int32_t v = *p & 255U;
    return v >= 128 ? v - 256 : v;
}
static float qjs_ta_f32(const uint8_t *p) {
    union { float f; uint32_t u; } v;
    v.u = get_u32(p); return v.f;
}
static double qjs_ta_f64(const uint8_t *p) {
    JSFloat64Union v;
    v.u64 = get_u64(p); return v.d;
}
static void qjs_ta_put_f32(uint8_t *p, float f) {
    union { float f; uint32_t u; } v;
    v.f = f; put_u32(p, v.u);
}
static void qjs_ta_put_f64(uint8_t *p, double f) {
    JSFloat64Union v;
    v.d = f; put_u64(p, v.u64);
}
static void qjs_ta_put_bits(uint8_t *p, uint64_t v, int width) {
    int i;
    for (i = 0; i < width; ++i) { p[i] = v & 255U; v >>= 8; }
}

'''
    source = replace(source, 'static JSValue JS_GetPropertyValue(JSContext *ctx, JSValueConst this_obj,',
                     helpers + 'static JSValue JS_GetPropertyValue(JSContext *ctx, JSValueConst this_obj,')
    readers = {
        'int8': ('qjs_ta_i8', 1), 'uint8': ('qjs_ta_u8', 1),
        'int16': ('qjs_ta_i16', 2), 'uint16': ('get_u16', 2),
        'int32': ('qjs_ta_i32', 4), 'uint32': ('get_u32', 4),
        'int64': ('qjs_ta_i64', 8), 'uint64': ('get_u64', 8),
        'fp16': ('get_u16', 2), 'float': ('qjs_ta_f32', 4), 'double': ('qjs_ta_f64', 8),
    }
    # These signed casts are native integer conversions, not pointer casts.
    source = replace(source, '/* Logical ArrayBuffer elements, independent of CHAR_BIT. */',
                     '''/* Logical ArrayBuffer elements, independent of CHAR_BIT. */
static uint32_t qjs_ta_u8(const uint8_t *p) { return *p & 255U; }
static int32_t qjs_ta_i16(const uint8_t *p) { return (int16_t)get_u16(p); }
static int32_t qjs_ta_i32(const uint8_t *p) { return (int32_t)get_u32(p); }
static int64_t qjs_ta_i64(const uint8_t *p) { return (int64_t)get_u64(p); }''')
    def get(body):
        for field, (load, width) in readers.items():
            body = replace(body, f'p->u.array.u.{field}_ptr[idx]',
                           f'{load}(p->u.array.u.uint8_ptr + (size_t)idx * {width})')
        return body
    source = transform_function(source, 'JS_GetPropertyValue', get)

    def put(body):
        for field, call, width, value, count in (
            ('uint8', 'qjs_ta_put_bits', 1, 'v, 1', 2),
            ('uint16', 'put_u16', 2, 'v', 1), ('uint32', 'put_u32', 4, 'v', 1),
            ('uint64', 'put_u64', 8, 'v', 1), ('fp16', 'put_u16', 2, 'tofp16(d)', 1),
            ('float', 'qjs_ta_put_f32', 4, 'd', 1), ('double', 'qjs_ta_put_f64', 8, 'd', 1),
        ):
            rhs = 'tofp16(d)' if field == 'fp16' else ('d' if field in ('float', 'double') else 'v')
            body = replace(body, f'p->u.array.u.{field}_ptr[idx] = {rhs};',
                           f'{call}(p->u.array.u.uint8_ptr + (size_t)idx * {width}, {value});', count)
        return body
    source = transform_function(source, 'JS_SetPropertyValue', put)

    def fill(body):
        start = body.index('    switch(shift) {')
        return body[:start] + '''    for (; k < final; ++k)
        qjs_ta_put_bits(p->u.array.u.uint8_ptr + ((size_t)k << shift), v64, 1 << shift);
    return JS_DupValue(ctx, this_val);
'''
    source = transform_function(source, 'js_typed_array_fill', fill)

    source = transform_function(source, 'js_typed_array_reverse', lambda body: '''
    JSObject *p;
    int len, i, j, width;
    uint8_t *data, tmp;
    len = js_typed_array_get_length_unsafe(ctx, this_val);
    if (len < 0) return JS_EXCEPTION;
    p = JS_VALUE_GET_OBJ(this_val);
    width = 1 << typed_array_size_log2(p->class_id);
    data = p->u.array.u.uint8_ptr;
    for (i = 0; i < len / 2; ++i) {
        uint8_t *a = data + (size_t)i * width;
        uint8_t *b = data + (size_t)(len - 1 - i) * width;
        for (j = 0; j < width; ++j) { tmp = a[j]; a[j] = b[j]; b[j] = tmp; }
    }
    return JS_DupValue(ctx, this_val);
''')

    # Keep upstream index/range and resize side-effect handling. A value-based
    # scan also preserves strict equality vs SameValueZero (NaN, signed zero,
    # BigInt and nonnumeric searches) without native-pointer special cases.
    def search(body):
        start = body.index('    is_bigint = 0;')
        end = body.index('\ndone:', start)
        return body[:start] + '''    for (; k != stop; k += inc) {
        JSValue element = JS_GetPropertyUint32(ctx, this_val, k);
        int found;
        if (JS_IsException(element)) goto exception;
        found = special == special_includes ? js_same_value_zero(ctx, argv[0], element)
                                           : js_strict_eq(ctx, argv[0], element);
        JS_FreeValue(ctx, element);
        if (found) { res = k; break; }
    }
''' + body[end:]
    source = transform_function(source, 'js_typed_array_indexOf', search)

    # Sort comparators/getters receive pointers into logical element copies.
    # Do not rewrite the native uint32_t index table in js_TA_cmp_generic.
    for kind, field in [('int8', 'int8'), ('uint8', 'uint8'), ('int16', 'int16'),
                        ('uint16', 'uint16'), ('int32', 'int32'), ('uint32', 'uint32'),
                        ('int64', 'int64'), ('uint64', 'uint64'),
                        ('float16', 'fp16'), ('float32', 'float'), ('float64', 'double')]:
        load = readers[field][0]
        def scalar(body, kind=kind, load=load):
            ctype = {'float16': 'uint16_t', 'float32': 'float', 'float64': 'double'}.get(kind, kind + '_t')
            for arg in ('a', 'b'):
                body = body.replace(f'*(const {ctype} *){arg}', f'{load}((const uint8_t *){arg})')
                body = body.replace(f'*({ctype} *){arg}', f'{load}((const uint8_t *){arg})')
            return body
        source = transform_function(source, 'js_TA_cmp_' + kind, scalar)
        source = transform_function(source, 'js_TA_get_' + kind, scalar)
    # uint16 subtraction must widen before C16 unsigned/int promotion.
    source = transform_function(source, 'js_TA_cmp_uint16', lambda body: body.replace(
        'return get_u16((const uint8_t *)a) - get_u16((const uint8_t *)b);',
        'return (int32_t)get_u16((const uint8_t *)a) - (int32_t)get_u16((const uint8_t *)b);'))

    def sorted_copy(body):
        start = body.index('                switch(elt_size) {')
        end = body.index('\n            }\n            js_free(ctx, array_idx);', start)
        return body[:start] + '''                for (i = 0; i < len; ++i) {
                    j = array_idx[i];
                    memcpy((uint8_t *)array_ptr + i * elt_size,
                           (uint8_t *)array + j * elt_size, elt_size);
                }''' + body[end:]
    source = transform_function(source, 'js_typed_array_sort', sorted_copy)

    # cutils accessors describe little-endian octets on the guest, regardless
    # of native scalar layout. DataView's default is big endian.
    for name in ('js_dataview_getValue', 'js_dataview_setValue'):
        source = transform_function(source, name, lambda body: replace(
            body, 'is_swap = littleEndian ^ !is_be();', 'is_swap = !littleEndian;'))
    source = transform_function(source, 'js_dataview_getValue', lambda body: replace(
        replace(body, '*(int8_t *)ptr', 'qjs_ta_i8(ptr)'), '*(uint8_t *)ptr', 'qjs_ta_u8(ptr)'))
    source = transform_function(source, 'js_dataview_setValue', lambda body: replace(
        body, '*ptr = v;', '*ptr = v & 255U;'))
    return source
