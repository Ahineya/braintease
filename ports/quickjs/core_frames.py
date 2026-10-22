"""Replace the four GNU alloca sites with checked, allocator-accounted frames.

This transformation also runs against the otherwise pristine host engine so
frame lifetime, exception cleanup and generator suspension can be tested before
C16 core execution works. It does not disable the existing native stack guard.
"""
from adapt import replace, _C_TOKENS

def transform_function(source, name, transform):
    import re
    match=re.search(r'(?m)^[A-Za-z_][\w *]*?\b'+re.escape(name)+r'\([^;{}]*\)\s*\{',source)
    if not match: raise ValueError('Missing core function: '+name)
    depth=1; end=None
    for token in _C_TOKENS.finditer(source,match.end()):
        if token[0]=='{':depth+=1
        elif token[0]=='}':depth-=1
        if not depth:end=token.start();break
    if end is None:raise ValueError('Unclosed core function: '+name)
    return source[:match.end()]+transform(source[match.end():end])+source[end:]

def heap_frames(source):
    source=replace(source, 'struct JSRuntime {',
                   '''struct JSRuntime {
    size_t c16_frame_cells; /* active call frames charged to the stack budget */
    struct QJSCallFrameHeader *c16_frame_cache[5]; /* at most 992 allocation units */''')
    source=replace(source, 'typedef struct JSCFunctionDataRecord {', """static JSValue JS_ThrowStackOverflow(JSContext *ctx);
typedef struct QJSCallFrameHeader { size_t charge; size_t capacity; } QJSCallFrameHeader;
static int js_c16_frame_bucket(size_t charge) {
    return charge <= 32 ? 0 : charge <= 64 ? 1 : charge <= 128 ? 2 :
           charge <= 256 ? 3 : charge <= 512 ? 4 : 5;
}
static void *js_c16_frame_alloc(JSContext *ctx, size_t size) {
    JSRuntime *rt = ctx->rt;
    QJSCallFrameHeader *header;
    size_t charge;
    int bucket;
    if (size > SIZE_MAX - sizeof(*header)) {
        JS_ThrowOutOfMemory(ctx); return NULL;
    }
    charge = (size ? size : 1) + sizeof(*header);
    if (charge > SIZE_MAX - rt->c16_frame_cells ||
        js_check_stack_overflow(rt, charge + rt->c16_frame_cells)) {
        JS_ThrowStackOverflow(ctx); return NULL;
    }
    /* A lowered budget must still stop calls, even if a frame is cached. */
    if (rt->malloc_ctx.malloc_state.malloc_size > rt->malloc_ctx.malloc_state.malloc_limit) {
        JS_ThrowOutOfMemory(ctx); return NULL;
    }
    bucket = js_c16_frame_bucket(charge);
    header = bucket < 5 ? rt->c16_frame_cache[bucket] : NULL;
    if (header && header->capacity >= charge) {
        rt->c16_frame_cache[bucket] = NULL;
    } else {
        /* Allocate the exact request, so size classes do not turn a valid
         * near-limit allocation into an avoidable out-of-memory failure. */
        header = js_malloc(ctx, charge);
        if (!header) return NULL;
        header->capacity = charge;
    }
    header->charge = charge;
    rt->c16_frame_cells += charge;
    return header + 1;
}
static void js_c16_frame_free(JSRuntime *rt, void *ptr) {
    QJSCallFrameHeader *header, *old;
    int bucket;
    if (!ptr) return;
    header = (QJSCallFrameHeader *)ptr - 1;
    assert(header->charge <= rt->c16_frame_cells);
    rt->c16_frame_cells -= header->charge;
    bucket = js_c16_frame_bucket(header->capacity);
    if (bucket < 5) {
        old = rt->c16_frame_cache[bucket];
        if (!old || old->capacity < header->capacity) {
            rt->c16_frame_cache[bucket] = header;
            js_free_rt(rt, old);
            return;
        }
    }
    js_free_rt(rt, header);
}

typedef struct JSCFunctionDataRecord {""")
    def data(body):
        body=replace(body,'    int i;', '    int i;\n    JSValue ret;\n    JSValue *owned_args = NULL;')
        body=replace(body,'        arg_buf = alloca(sizeof(arg_buf[0]) * s->length);', '''        if (js_check_stack_overflow(ctx->rt, sizeof(arg_buf[0]) * s->length))
            return JS_ThrowStackOverflow(ctx);
        owned_args = js_c16_frame_alloc(ctx, sizeof(arg_buf[0]) * s->length);
        if (!owned_args) return JS_EXCEPTION;
        arg_buf = owned_args;''')
        return replace(body,'    return s->func(ctx, this_val, argc, arg_buf, s->magic, s->data);', '''    ret = s->func(ctx, this_val, argc, arg_buf, s->magic, s->data);
    js_c16_frame_free(ctx->rt, owned_args);
    return ret;''')
    source=transform_function(source,'js_c_function_data_call',data)
    def native(body):
        body=replace(body,'    JSValueConst *arg_buf;', '    JSValueConst *arg_buf;\n    JSValue *owned_args = NULL;')
        body=replace(body, '    prev_sf = rt->current_stack_frame;', """    if (unlikely(argc < arg_count)) {
        owned_args = js_c16_frame_alloc(ctx, sizeof(arg_buf[0]) * arg_count);
        if (!owned_args) return JS_EXCEPTION;
    }
    prev_sf = rt->current_stack_frame;""")
        body=replace(body,'        arg_buf = alloca(sizeof(arg_buf[0]) * arg_count);',
                     '        arg_buf = owned_args;')
        return replace(body,'    rt->current_stack_frame = sf->prev_frame;',
                       '    rt->current_stack_frame = sf->prev_frame;\n    js_c16_frame_free(rt, owned_args);')
    source=transform_function(source,'js_call_c_function',native)
    def bound(body):
        body=replace(body,'    int arg_count, i;', '    int arg_count, i;\n    JSValue ret;')
        body=replace(body,'    arg_count = bf->argc + argc;', '''    if (argc < 0 || bf->argc < 0 || argc > INT32_MAX - bf->argc ||
        (size_t)argc > SIZE_MAX / sizeof(JSValue) ||
        (size_t)bf->argc > SIZE_MAX / sizeof(JSValue) - (size_t)argc)
        return JS_ThrowOutOfMemory(ctx);
    arg_count = bf->argc + argc;''')
        body=replace(body,'    arg_buf = alloca(sizeof(JSValue) * arg_count);', '''    arg_buf = js_c16_frame_alloc(ctx, arg_count ? sizeof(JSValue) * arg_count : 1);
    if (!arg_buf) return JS_EXCEPTION;''')
        body=replace(body,'        return JS_CallConstructor2(', '        ret = JS_CallConstructor2(')
        body=replace(body,'        return JS_Call(ctx,', '        ret = JS_Call(ctx,')
        return body+'    js_c16_frame_free(ctx->rt, (void *)arg_buf);\n    return ret;\n'
    source=transform_function(source,'js_call_bound_function',bound)
    def internal(body):
        body=replace(body,'    size_t alloca_size;', '    size_t alloca_size;\n    JSValue *owned_locals = NULL;')
        body=replace(body,'    local_buf = alloca(alloca_size);', '''    owned_locals = js_c16_frame_alloc(caller_ctx, alloca_size ? alloca_size : 1);
    if (!owned_locals) return JS_EXCEPTION;
    local_buf = owned_locals;''')
        return replace(body,'    rt->current_stack_frame = sf->prev_frame;','''    rt->current_stack_frame = sf->prev_frame;
    /* Generator/async frames are owned by their suspended state, not this call. */
    js_c16_frame_free(rt, owned_locals);''')
    source=transform_function(source,'JS_CallInternal',internal)
    source=transform_function(source, 'JS_FreeRuntime', lambda body: '''
    int c16_bucket;
    assert(rt->c16_frame_cells == 0);
    for (c16_bucket = 0; c16_bucket < 5; ++c16_bucket) {
        js_free_rt(rt, rt->c16_frame_cache[c16_bucket]);
        rt->c16_frame_cache[c16_bucket] = NULL;
    }
'''+body)
    if 'alloca(' in source: raise ValueError('Unexpected remaining alloca site')
    return source
