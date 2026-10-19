"""Checked adaptations for NetSurf's deliberately limited local-page profile."""
from adapt import replace
import re


def adapt_core(name, source):
    if name.startswith('netsurf/'):
        source = '#include <netsurf_profile.h>\n' + source
        # The guest SDK declares strndup's length as int, while NetSurf's
        # fallback uses size_t. Keep one correct ABI under a private name.
        for function in ('strtoll', 'strtoull', 'strtof', 'ceilf', 'strndup'):
            source = re.sub(r'\b' + function + r'\b', 'ns_' + function, source)
    if name == 'netsurf/content/mimesniff.c':
        # Recorded RCC high-octet string-literal defect (test_string_hex_octets).
        # Numeric octet arrays keep signatures, embedded NULs and SLEN exact.
        literals = re.findall(r'"(?:[^"\\]|\\.)*"', source)
        arrays = []
        for literal in dict.fromkeys(literals):
            if r'\x' not in literal:
                continue
            value = bytes(literal[1:-1], 'ascii').decode('unicode_escape').encode('latin1')
            symbol = 'c16_signature_' + str(len(arrays))
            arrays.append('static const ns_octet_t ' + symbol + '[] = {' +
                          ','.join(str(c) for c in value) + ',0};')
            source = source.replace(literal, symbol)
        source = source.replace('struct map_s {', '\n'.join(arrays) + '\nstruct map_s {')
        source = replace(source, '(data[0] << 24) | (data[1] << 16)',
                         '((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16)')
    if name == 'netsurf/content/fetch.c':
        source = replace(source, '#include "content/fetchers/curl.h"', '#ifdef WITH_CURL\n#include "content/fetchers/curl.h"\n#endif')
    if name == 'netsurf/content/handlers/css/hints.c':
        # HTML dimension attributes use this separate numeric parser rather
        # than LibCSS's adapted parser. 1<<21/22 is invalid on C16 int and
        # collapsed ordinary width/height hints to zero. Keep its arithmetic
        # explicitly wide, including legacy HTML colour attributes.
        source = source.replace('(1 << ', '(1L << ')
        source = replace(source, '(intpart << 10) | fracpart', '(intpart * 1024L) | fracpart')
        source = replace(source, '(0xff << 24) | (r << 16) | (g << 8) | b',
                         '(0xffUL << 24) | ((uint32_t)r << 16) | ((uint32_t)g << 8) | b', 2)
    if name == 'netsurf/content/fetchers/resource.c':
        # Guest printf has 32-bit long formatting, but not long long. This
        # synthetic ETag is a fixed 32-bit value, independent of host time_t.
        source = replace(source, '"ETag: \\"%10" PRId64 "\\""', '"ETag: \\"%10ld\\""')
        source = replace(source, '(int64_t) DIRECT_ETAG_VALUE', '(long) DIRECT_ETAG_VALUE')
    if name in ('netsurf/utils/regex.h', 'netsurf/utils/utils.c'):
        # Same C99 array-qualifier defect as the LibCSS bloom parameters.
        source = source.replace('[restrict]', '[]')
    if name == 'netsurf/utils/regex.h':
        source = replace(source, '#define REG_NOMATCH 1', '''/* FUZIX exports the incompatible historical V8 regex API. */
#define regcomp ns_port_regcomp
#define regexec ns_port_regexec
#define regerror ns_port_regerror
#define regfree ns_port_regfree
#define REG_NOMATCH 1''')
    if name == 'netsurf/include/netsurf/types.h':
        # The unused convenience alias hides ordinary locals throughout the
        # renderer. Keep the public struct tag and layout (recorded RCC bug).
        source = replace(source, 'typedef struct rect {', 'struct rect {')
        source = replace(source, '} rect;', '};')
    if name == 'netsurf/include/netsurf/inttypes.h':
        # size_t is unsigned long on C16; the SDK scanner has no z modifier.
        source = replace(source, '#define PRIsizet "zu"',
                         '#if CHAR_BIT == 16\n#define PRIsizet "lu"\n#else\n#define PRIsizet "zu"\n#endif')
    if name == 'netsurf/utils/time.c':
        # Guest time_t is 32-bit. Host long also preserves its native time_t.
        source = replace(source, 'long long val;', 'long val;')
        source = replace(source, 'val = (long long)*timep;', 'val = (long)*timep;')
        source = replace(source, '"%lld", val', '"%ld", val')
    if name == 'netsurf/include/netsurf/plot_style.h':
        source = replace(source, '((v) << PLOT_STYLE_RADIX)', '((int32_t)(v) << PLOT_STYLE_RADIX)')
    if name == 'netsurf/content/handlers/text/textplain.c':
        # Widen before multiplying: 128 * 1024 wraps to zero on C16,
        # collapsing every plain-text baseline onto the same row.
        source = replace(source, '(nsoption_int(font_size) * PLOT_STYLE_SCALE)',
                         '((int32_t)nsoption_int(font_size) * PLOT_STYLE_SCALE)')
        # Local files and unlabelled text use the system UTF-8 convention.
        # The handler still honours an explicit Content-Type charset.
        source = replace(source, 'lwc_intern_string("Windows-1252",\n\t\t\t\t   SLEN("Windows-1252"),',
                         'lwc_intern_string("UTF-8",\n\t\t\t\t   SLEN("UTF-8"),')
        source = replace(source, 'static const ns_octet_t *u_fffd = (const ns_octet_t *) "\\xef\\xbf\\xbd";',
                         'static const ns_octet_t u_fffd[] = {0xef, 0xbf, 0xbd};')
        start = source.index('static void textplain_reformat(')
        end = source.index('\n\n/**\n * Destroy a CONTENT_TEXTPLAIN', start)
        source = source[:start] + '#include <textplain-reformat.inc>\n' + source[end:]
    if name == 'netsurf/desktop/bitmap.h':
        source = re.sub(r'\(pixel\[bitmap_layout\.([rgba])\] <<', r'((uint32_t)pixel[bitmap_layout.\1] <<', source)
    if name == 'netsurf/content/fetchers/about/nscolours.c':
        # A colour is 32 bits, whereas %x consumes a 16-bit unsigned int on
        # C16. Otherwise the following %s reads half a colour as its pointer.
        source = replace(source, '#%06x;', '#%06lx;')
        source = replace(source, 'colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_BG]),',
                         '(unsigned long)colour_rb_swap(nscolours[NSCOLOUR_WIN_ODD_BG]),')
    if name == 'netsurf/desktop/local_history.c':
        source = replace(source, 'rect cursor;', 'struct rect cursor;')
    if name == 'netsurf/desktop/browser_window.c':
        # Only these two favicon functions use a variable hiding nsurl.
        start = source.index('static nserror\nbrowser_window_favicon_callback')
        end = source.index('\nstatic void browser_window_refresh(', start)
        section = source[start:end]
        section = re.sub(r'\bnsurl\b', 'icon_url', section)
        section = section.replace('icon_url *', 'nsurl *')
        source = source[:start] + section + source[end:]
        source = replace(source, 'if (bw->window == NULL) {\n\t\t/* Not top-level browser window; not interested */',
                         'if (true) {\n\t\t/* This frontend does not display favicons. */')
        source = replace(source, 'content_type content_type;', 'content_type page_type;')
        source = replace(source, 'content_type = content_get_type', 'page_type = content_get_type')
        source = replace(source, 'if (content_type != CONTENT_HTML && content_type != CONTENT_TEXTPLAIN)',
                         'if (page_type != CONTENT_HTML && page_type != CONTENT_TEXTPLAIN)')
    if name == 'netsurf/desktop/frames.c':
        # C99 VLAs are recorded by test_variable_length_matrix. Bounded
        # framesets avoid unbounded stack use as well as the compiler gap.
        source = replace(source, 'int widths[bw->cols][bw->rows];', 'int widths[16][16];')
        source = replace(source, 'int heights[bw->cols][bw->rows];', 'int heights[16][16];')
        source = replace(source, '\t/* 1. Create children */', '''    if (frameset->cols < 1 || frameset->rows < 1 ||
        frameset->cols > 16 || frameset->rows > 16) return NSERROR_BAD_SIZE;

\t/* 1. Create children */''')
    if name == 'netsurf/desktop/netsurf.c':
        source = replace(source, '#define MINIMUM_MEMORY_CACHE_SIZE (2 * 1024 * 1024)',
                         '#define MINIMUM_MEMORY_CACHE_SIZE 1048576UL\n#include <c16-cache.h>')
        source = replace(source, '#define LLCACHE_STORE_MIN_BANDWIDTH (128 * 1024)',
                         '#define LLCACHE_STORE_MIN_BANDWIDTH 0UL')
        source = replace(source, '#define LLCACHE_STORE_MAX_BANDWIDTH (1024 * 1024)',
                         '#define LLCACHE_STORE_MAX_BANDWIDTH 1048576UL')
        source = replace(source, 'hlcache_parameters.llcache.store.limit = nsoption_uint(disc_cache_size);',
                         'hlcache_parameters.llcache.store.limit = C16_CACHE_LIMIT;')
    if name == 'netsurf/content/llcache.c':
        source = replace(source, '#include "content/backing_store.h"',
                         '#include "content/backing_store.h"\n#include <c16-cache.h>')
        source = replace(source, 'object->source_len + len + 64 * 1024',
                         'object->source_len + len + 65536UL')
        # C16 int cannot hold ordinary max-age=86400 values or epoch deltas.
        source = replace(source, 'int age;', 'long age;')
        source = replace(source, 'int max_age;', 'long max_age;')
        source = source.replace('int remaining_lifetime;', 'long remaining_lifetime;')
        source = replace(source, 'static int\nllcache_object_rfc2616_remaining_lifetime',
                         'static long\nllcache_object_rfc2616_remaining_lifetime')
        source = replace(source, 'int current_age, freshness_lifetime;', 'long current_age, freshness_lifetime;')
        source = replace(source, '"%d:%d", freshness_lifetime, current_age',
                         '"%ld:%ld", freshness_lifetime, current_age')
        source = replace(source, '"%p: (%d > 0 || %d != %d)"',
                         '"%p: (%ld > 0 || %d != %d)"')
        source = replace(source, 'object->cache.age = atoi(value);',
                         'object->cache.age = strtoul(value, NULL, 10) > 2147483647UL ? 2147483647L : atol(value);')
        source = replace(source, 'object->cache.max_age = http_cache_control_max_age(cc);',
                         'object->cache.max_age = http_cache_control_max_age(cc) > 2147483647UL ? 2147483647L : (long)http_cache_control_max_age(cc);')
        source = replace(source, 'char *etag;', 'bool no_store;\n\tchar *etag;')
        source = replace(source, '\thttp_cache_control_destroy(cc);', '''
        if (http_cache_control_no_store(cc)) {
            object->cache.no_store = true;
            guit->llcache->invalidate(object->url);
        }
\thttp_cache_control_destroy(cc);''')
        source = replace(source, "\t/* Append header data to the object's headers array */", '''
        /* The worker always requests identity encoding. Other Vary fields
         * require request-header keys that this bounded backend lacks. */
        if ((!strcasecmp(name, "Vary") && strcasecmp(value, "Accept-Encoding")) ||
            !strcasecmp(name, "Set-Cookie")) {
            object->cache.no_store = true;
            object->cache.no_cache = LLCACHE_VALIDATE_ALWAYS;
            guit->llcache->invalidate(object->url);
        }
\t/* Append header data to the object's headers array */''')
        source = replace(source, '\treturn NSERROR_OK;\n}\n\n/**\n * Remove a low-level cache object',
                         '\tdestination->cache.no_store |= source->cache.no_store;\n\treturn NSERROR_OK;\n}\n\n/**\n * Remove a low-level cache object')
        source = replace(source, '/**\n * Core llcache control context.',
                         '#include <c16-cache-policy.inc>\n\n/**\n * Core llcache control context.')
        source = replace(source, '(remaining_lifetime > llcache->minimum_lifetime)',
                         'c16_persistable(object, remaining_lifetime)')
        source = replace(source, 'guit->misc->schedule(5000, llcache_persist, NULL);',
                         'guit->misc->schedule(500, llcache_persist, NULL);')
        # Shutdown drains hlcache before finalising llcache. Save validators
        # before that cleanup, and retain unpersisted candidates until a later
        # writeout (or ordinary RAM-pressure eviction) can deal with them.
        source = replace(source, '\tNSLOG(llcache, DEBUG, "Attempting cache clean");',
                         '\tllcache_persist(NULL);\n\tNSLOG(llcache, DEBUG, "Attempting cache clean");')
        source = replace(source, '(remaining_lifetime <= 0))',
                         '(remaining_lifetime <= 0) &&\n            !(object->store_state == LLCACHE_STATE_RAM &&\n              c16_cache_enabled() && c16_persistable(object, remaining_lifetime)))')
        source = replace(source, '\t/* Attempt to persist anything we have left lying around */\n\tllcache_persist(NULL);', '''\t/* Drain successful batches; stop on refusal/full storage, without spinning. */
    {
        uint64_t before;
        do {
            before = llcache->total_written;
            llcache_persist(NULL);
        } while (llcache->total_written != before);
    }''')
        source = replace(source, '\t\tobject->fetch.state = LLCACHE_FETCH_COMPLETE;\n\t\tobject->fetch.fetch = NULL;\n\n\t\t/* Shrink source buffer', '''
        /* A complete replacement response supersedes the old disk generation,
         * even while its source buffer is pinned by an existing page. */
        guit->llcache->invalidate(object->url);
        if (fetch_http_code(object->fetch.fetch) != 200) object->cache.no_store = true;
\t\tobject->fetch.state = LLCACHE_FETCH_COMPLETE;
\t\tobject->fetch.fetch = NULL;

\t\t/* Shrink source buffer''')
        # Keep stale disk objects for conditional requests after RAM cleanup.
        source = replace(source, '''\t\t\t\tif (object->store_state == LLCACHE_STATE_DISC) {
\t\t\t\t\tguit->llcache->invalidate(object->url);
\t\t\t\t}
''', '')
        # After adoption, all failure paths must release data through the store.
        source = replace(source, '\tret = llcache_serialise_metadata(object,',
                         '\tobject->store_state = LLCACHE_STATE_DISC;\n\tret = llcache_serialise_metadata(object,')
        source = replace(source, '\tguit->llcache->release(object->url, BACKING_STORE_META);\n\tif (ret != NSERROR_OK)',
                         '\tif (ret == NSERROR_OK) guit->llcache->release(object->url, BACKING_STORE_META);\n\telse free(metadata);\n\tif (ret != NSERROR_OK)')
    if name == 'netsurf/desktop/global_history.c':
        source = replace(source, '#define N_SEC_PER_DAY (60 * 60 * 24)', '#define N_SEC_PER_DAY 86400UL')
        start = source.index('\t/* get the time at the start of today */')
        end = source.index('\n\tgh_ctx.today = t;', start)
        source = source[:start] + '''    /* Guest libc uses UTC for local time. Group days in UTC without
     * depending on the unavailable mktime normalization API. */
    full_time = gmtime(&t);
    if (full_time == NULL) return NSERROR_UNKNOWN;
    t -= t % N_SEC_PER_DAY;
''' + source[end:]
    if name == 'netsurf/utils/talloc.c':
        # The allocator validates a 32-bit magic, not a native-int flag word.
        source = replace(source, '\tunsigned flags;', '\tuint32_t flags;')
    if name == 'netsurf/desktop/page-info.c':
        # Same nested union static-initializer regression as LibDOM strings.
        source = replace(source, '\tunion {', '\tstruct {')
        # The domain is a TEXT entry; do not rely on overlapping alternatives.
        source = replace(source, '.u.item.item.height / 2;', '.u.text.height / 2;')
    if name == 'netsurf/desktop/options.h':
        source = replace(source, 'memory_cache_size, 12 * 1024 * 1024', 'memory_cache_size, 32767')
        source = replace(source, 'disc_cache_size, 1024 * 1024 * 1024', 'disc_cache_size, 0')
    if name == 'netsurf/utils/nsoption.h':
        # Same recorded nested-union initializer defect as LibDOM strings.
        source = replace(source, '\tunion {', '\tstruct {')
    if name == 'netsurf/utils/nsoption.c':
        source = replace(source, '{ .cs = DEFAULT }', '{ .s = DEFAULT }')
    if name == 'netsurf/content/handlers/html/redraw.c':
        source = replace(source, 'css_color colour;', 'css_color paint_colour;')
        source = replace(source, '&colour)', '&paint_colour)')
        source = replace(source, 'nscss_color_is_transparent(colour)', 'nscss_color_is_transparent(paint_colour)')
        source = replace(source, 'colour colour,', 'colour paint_colour,', 2)
        source = replace(source, '.stroke_colour = colour,', '.stroke_colour = paint_colour,', 2)
        source = replace(source, 'scale, colour, ratio, ctx)', 'scale, paint_colour, ratio, ctx)')
    if name == 'netsurf/utils/utf8.c':
        source = replace(source, '#include <iconv.h>', '#include <parserutils/charset/codec.h>')
        start = source.index('/* Cache of previous iconv')
        end = source.index('/* exported interface documented in utils/utf8.h */\nbool utf8_save_text', start)
        source = source[:start] + '#include <utf8-convert.inc>\n\n' + source[end:]
    if name == 'netsurf/utils/log.c':
        start = source.index('/** Subtract the `struct timeval')
        end = source.index('\n#ifdef WITH_NSLOG', start)
        source = source[:start] + '''/* Logging uses the same monotonic clock as the scheduler. */
#include <nsutils/time.h>
static const char *nslog_gettime(void)
{
    static char buffer[32];
    uint64_t milliseconds;
    if (nsu_getmonotonic_ms(&milliseconds) != NSUERROR_OK) return "(clock unavailable)";
    snprintf(buffer, sizeof(buffer), "(%lu.%03u)",
             (unsigned long)(milliseconds / 1000), (unsigned)(milliseconds % 1000));
    return buffer;
}
''' + source[end:]
    if name == 'netsurf/utils/filepath.c':
        # Resource lookup only needs readable paths; the kernel resolves
        # symlinks when opening. Do not pretend this canonicalises paths.
        source = source.replace('realpath(', 'ns_readable_resource_path(')
        start = source.index('/* exported interface documented in filepath.h */')
        source = source[:start] + '''static char *ns_readable_resource_path(const char *path, char *buffer)
{
    if (strlen(path) >= PATH_MAX) { errno = ENAMETOOLONG; return NULL; }
    if (access(path, R_OK) != 0) return NULL;
    if (buffer == NULL) return strdup(path);
    strcpy(buffer, path);
    return buffer;
}

''' + source[start:]
    if name == 'netsurf/content/fetchers/file/file.c':
        source = replace(source, '#define FETCH_FILE_MAX_BUF_SIZE (1024 * 1024)', '#define FETCH_FILE_MAX_BUF_SIZE 4096')
        source = replace(source, 'buf = malloc(buf_size);', 'buf = malloc(buf_size ? buf_size : 1);')
        source = replace(source, 'ssize_t tot_read = 0;', 'off_t tot_read = 0;')
        start = source.index('\t\t/* Get date in output format.')
        end = source.index('\n\t}\n', start)
        source = source[:start] + '''        struct tm *stamp = localtime(&ent_stat.st_mtime);
        if (stamp == NULL) { datebuf[0] = timebuf[0] = 0; }
        else {
            snprintf(datebuf, sizeof(datebuf), "%04d-%02d-%02d",
                     stamp->tm_year + 1900, stamp->tm_mon + 1, stamp->tm_mday);
            snprintf(timebuf, sizeof(timebuf), "%02d:%02d", stamp->tm_hour, stamp->tm_min);
        }''' + source[end:]
    if name == 'netsurf/utils/config.h':
        source += '''
#ifdef RIPPLE_MMU
#undef HAVE_MMAP
#undef WITH_MMAP
#undef HAVE_REGEX
#undef HAVE_DIRFD
#undef HAVE_UNLINKAT
#undef HAVE_FSTATAT
#undef HAVE_SCANDIR
#undef HAVE_STRPTIME
#undef HAVE_STRFTIME
#undef HAVE_STRNDUP
char *ns_strndup(const char *, size_t);
#define NO_IPV6
#endif
'''
    if name == 'netsurf/utils/messages.c':
        source = replace(source, '#include <zlib.h>', '')
    if name == 'netsurf/utils/log.h':
        # GNU named variadics are recorded by test_named_variadic_macro.
        # Taking the format and arguments together uses plain C99 variadics
        # and needs neither the GNU spelling nor comma-swallowing extension.
        source = replace(source, 'NSLOG(catname, level, logmsg, args...)', 'NSLOG(catname, level, ...)')
        source = replace(source, 'logmsg , ##args', '__VA_ARGS__')
    if name == 'netsurf/utils/hashtable.c':
        source = replace(source, '#include <zlib.h>', '#include <stdio.h>')
        start = source.index('static nserror\nhash_add_inline_gzip(')
        end = source.index('\n\n/* exported interface', start)
        source = source[:start] + '''static nserror
hash_add_inline_gzip(struct hash_table *ht, const ns_octet_t *data, size_t size)
{
    return NSERROR_NOT_IMPLEMENTED;
}
''' + source[end:]
        source = replace(source, 'gzFile fp;', 'FILE *fp;')
        source = replace(source, 'gzopen(path, "r")', 'fopen(path, "r")')
        source = replace(source, 'while (gzgets(fp, s, sizeof s))', 'while (fgets(s, sizeof s, fp))')
        source = replace(source, 'gzclose(fp)', 'fclose(fp)')
        source = replace(source, 'while (fgets(s, sizeof s, fp))', '''{
            int a = fgetc(fp), b = fgetc(fp);
            if (a == 0x1f && b == 0x8b) { fclose(fp); return NSERROR_NOT_IMPLEMENTED; }
            if (fseek(fp, 0, SEEK_SET)) { fclose(fp); return NSERROR_INVALID; }
        }
        while (fgets(s, sizeof s, fp))''')
        source = replace(source, 'if ((data[0]==0x1f)', 'if (size >= 2 && (data[0]==0x1f)')
        source = replace(source, 'if (slen > sizeof s)', 'if (slen >= sizeof s - 1)')
        source = replace(source, '\t\t\t\tslen = 0;\n', '\t\t\t\treturn NSERROR_BAD_SIZE;\n')
        source = replace(source, 's[--slen] = 0;  /* remove \\n at end */', "if (slen && s[slen - 1] == '\\n') s[--slen] = 0;")
        source = replace(source, 'int slen = strlen(s);', '''int slen = strlen(s);
            if (slen == sizeof(s) - 1 && s[slen - 1] != '\\n') {
                fclose(fp); return NSERROR_BAD_SIZE;
            }''')
        source = replace(source, '\n\tfclose(fp);\n\n\treturn res;', '\n\tif (ferror(fp)) res = NSERROR_INVALID;\n\tfclose(fp);\n\n\treturn res;')
        source = replace(source, '\n\tif (size >= 2 && (data[0]==0x1f)', '\n\tif (ht == NULL || (size && data == NULL)) return NSERROR_BAD_PARAMETER;\n\tif (size >= 2 && (data[0]==0x1f)')
    if name == 'libnsutils/src/base64.c':
        # Decoded octets must narrow explicitly on C16. Also guard empty
        # input before the upstream tail/padding probes.
        source = re.sub(r'(decoded\[opidx\+\+\] = )(.*?);', r'\1(\2) & 255;', source)
        source = replace(source, "if (input[input_length - 1] == '=')", "if (input_length >= 1 && input[input_length - 1] == '=')")
        source = replace(source, "if (input[input_length - 2] == '=')", "if (input_length >= 2 && input[input_length - 2] == '=')")
        source = replace(source, 'sextet[sextet_idx] = decoding_table[input[idx++]];', '''if (input[idx] > 255) { free(decoded); return NSUERROR_BAD_ENCODING; }
                sextet[sextet_idx] = decoding_table[input[idx++]];''')
        source = replace(source, 'decoded = malloc(decoded_len);', 'decoded = malloc(decoded_len ? decoded_len : 1);')
    if name == 'libnsutils/src/time.c':
        # Both guest libc implementations provide CLOCK_MONOTONIC. Avoid
        # the upstream wall-clock fallback and multiply in 64 bits.
        source = replace(source, '#include <unistd.h>', '#include <unistd.h>\n#define _POSIX_TIMERS 1\n#define _POSIX_MONOTONIC_CLOCK 1')
        source = source.replace('(tp.tv_sec * 1000)', '((uint64_t)tp.tv_sec * 1000)')
        source = replace(source, 'clock_gettime(CLOCK_MONOTONIC, &tp);',
                         'if (current_out == NULL) return NSUERROR_BAD_PARAMETER;\n    if (clock_gettime(CLOCK_MONOTONIC, &tp) != 0) return NSUERROR_INIT_FAILED;')
    return source
