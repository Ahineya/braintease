/* NetSurf charset bridge using the already ported ParserUtils codecs.
 * Strict conversion rejects unsupported encodings and malformed input.
 * No host-word casts: the intermediate scalars are serialized BE octets. */
#include <parserutils/charset/mibenum.h>
static nserror ns_convert(const char *text, size_t length, const char *from,
                          const char *to, char **result, size_t *result_length)
{
    parserutils_charset_codec *decoder = NULL, *encoder = NULL;
    parserutils_charset_codec_optparams option;
    parserutils_error error;
    ns_octet_t *pivot = NULL, *output = NULL, *write;
    const ns_octet_t *read;
    size_t remaining, capacity, used;
    nserror status = NSERROR_BAD_ENCODING;
    if (result == NULL || text == NULL || from == NULL || to == NULL)
        return NSERROR_BAD_PARAMETER;
    *result = NULL;
    if (result_length) *result_length = 0;
    if (!length) length = strlen(text);
    /* A bounded conversion, separate from the streaming HTML parser. */
    if (length > 65536UL) return NSERROR_BAD_SIZE;
    {
        size_t offset;
        for (offset = 0; offset < length; offset++)
            if ((unsigned char)text[offset] > 255) return NSERROR_BAD_ENCODING;
        if (parserutils_charset_mibenum_from_name(from, strlen(from)) == 106) {
            offset = 0;
            while (offset < length) {
                uint32_t scalar;
                size_t consumed;
                if (parserutils_charset_utf8_to_ucs4((const ns_octet_t *)text + offset,
                        length - offset, &scalar, &consumed) != PARSERUTILS_OK || !consumed)
                    return NSERROR_BAD_ENCODING;
                offset += consumed;
            }
        }
    }
    error = parserutils_charset_codec_create(from, &decoder);
    if (error != PARSERUTILS_OK)
        return error == PARSERUTILS_NOMEM ? NSERROR_NOMEM : NSERROR_BAD_ENCODING;
    error = parserutils_charset_codec_create(to, &encoder);
    if (error != PARSERUTILS_OK) {
        status = error == PARSERUTILS_NOMEM ? NSERROR_NOMEM : NSERROR_BAD_ENCODING;
        goto finished;
    }
    option.error_mode.mode = PARSERUTILS_CHARSET_CODEC_ERROR_STRICT;
    parserutils_charset_codec_setopt(decoder, PARSERUTILS_CHARSET_CODEC_ERROR_MODE, &option);
    parserutils_charset_codec_setopt(encoder, PARSERUTILS_CHARSET_CODEC_ERROR_MODE, &option);
    pivot = malloc(length * 4 + 4);
    output = malloc(length * 6 + 4);
    if (pivot == NULL || output == NULL) { status = NSERROR_NOMEM; goto finished; }
    read = (const ns_octet_t *)text;
    remaining = length;
    write = pivot;
    capacity = length * 4 + 4;
    error = parserutils_charset_codec_decode(decoder, &read, &remaining, &write, &capacity);
    if (error != PARSERUTILS_OK || remaining != 0) goto finished;
    /* Flush pending input; truncated multibyte sequences must fail. */
    error = parserutils_charset_codec_decode(decoder, &read, &remaining, &write, &capacity);
    if (error != PARSERUTILS_OK) goto finished;
    remaining = write - pivot;
    read = pivot;
    write = output;
    capacity = length * 6;
    error = parserutils_charset_codec_encode(encoder, &read, &remaining, &write, &capacity);
    if (error != PARSERUTILS_OK || remaining != 0) goto finished;
    used = write - output;
    memset(write, 0, 4);
    *result = (char *)output;
    if (result_length) *result_length = used;
    output = NULL;
    status = NSERROR_OK;
finished:
    if (encoder) parserutils_charset_codec_destroy(encoder);
    if (decoder) parserutils_charset_codec_destroy(decoder);
    free(pivot);
    free(output);
    return status;
}

nserror utf8_finalise(void) { return NSERROR_OK; }
nserror utf8_to_enc(const char *text, const char *encoding, size_t length, char **result)
{
    return ns_convert(text, length, "UTF-8", encoding, result, NULL);
}
nserror utf8_from_enc(const char *text, const char *encoding, size_t length,
                       char **result, size_t *result_length)
{
    return ns_convert(text, length, encoding, "UTF-8", result, result_length);
}

nserror utf8_to_html(const char *text, const char *encoding, size_t length, char **result)
{
    char *escaped, *write;
    size_t i;
    nserror status;
    if (text == NULL || result == NULL) return NSERROR_BAD_PARAMETER;
    *result = NULL;
    if (!length) length = strlen(text);
    if (length > 65536UL) return NSERROR_BAD_SIZE;
    escaped = malloc(length * 5 + 1);
    if (escaped == NULL) return NSERROR_NOMEM;
    write = escaped;
    for (i = 0; i < length; i++) {
        if (text[i] == '&') { memcpy(write, "&amp;", 5); write += 5; }
        else if (text[i] == '<') { memcpy(write, "&lt;", 4); write += 4; }
        else if (text[i] == '>') { memcpy(write, "&gt;", 4); write += 4; }
        else *write++ = text[i];
    }
    *write = 0;
    status = ns_convert(escaped, write - escaped, "UTF-8", encoding, result, NULL);
    free(escaped);
    return status;
}
