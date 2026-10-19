/* Included by dependencies.c: a small DOM adapter exercising real LibCSS
 * parsing, selector matching, cascade and computed-property storage. */
#include <libcss/libcss.h>

struct style_node {
    const char *tag, *id, *class_name;
    struct style_node *parent;
    void *cache;
};
static css_select_handler style_handler;
static bool matches(const char *text, lwc_string *string)
{
    return text && strlen(text) == lwc_string_length(string) &&
        !memcmp(text, lwc_string_data(string), lwc_string_length(string));
}
static css_error resolve_url(void *pw, const char *base, lwc_string *rel, lwc_string **abs)
{
    *abs = lwc_string_ref(rel); return CSS_OK;
}
static css_error node_name(void *pw, void *node, css_qname *name)
{
    struct style_node *n = node;
    name->ns = NULL;
    return lwc_intern_string(n->tag, strlen(n->tag), &name->name) == lwc_error_ok ? CSS_OK : CSS_NOMEM;
}
static css_error node_classes(void *pw, void *node, lwc_string ***classes, uint32_t *count)
{
    struct style_node *n = node;
    *count = 0; *classes = NULL;
    if (!n->class_name) return CSS_OK;
    *classes = malloc(sizeof(lwc_string *));
    if (!*classes) return CSS_NOMEM;
    if (lwc_intern_string(n->class_name, strlen(n->class_name), *classes) != lwc_error_ok) {
        free(*classes); *classes = NULL; return CSS_NOMEM;
    }
    *count = 1; return CSS_OK;
}
static css_error node_id(void *pw, void *node, lwc_string **id)
{
    struct style_node *n = node;
    *id = NULL;
    if (!n->id) return CSS_OK;
    return lwc_intern_string(n->id, strlen(n->id), id) == lwc_error_ok ? CSS_OK : CSS_NOMEM;
}
static css_error ancestor(void *pw, void *node, const css_qname *name, void **found)
{
    struct style_node *n = ((struct style_node *)node)->parent;
    while (n && !matches(n->tag, name->name)) n = n->parent;
    *found = n; return CSS_OK;
}
static css_error named_parent(void *pw, void *node, const css_qname *name, void **found)
{
    struct style_node *n = ((struct style_node *)node)->parent;
    *found = n && matches(n->tag, name->name) ? n : NULL; return CSS_OK;
}
static css_error no_named_sibling(void *pw, void *node, const css_qname *name, void **found)
{ *found = NULL; return CSS_OK; }
static css_error parent(void *pw, void *node, void **found)
{ *found = ((struct style_node *)node)->parent; return CSS_OK; }
static css_error no_sibling(void *pw, void *node, void **found)
{ *found = NULL; return CSS_OK; }
static css_error has_name(void *pw, void *node, const css_qname *name, bool *match)
{ *match = matches(((struct style_node *)node)->tag, name->name); return CSS_OK; }
static css_error has_class(void *pw, void *node, lwc_string *name, bool *match)
{ *match = matches(((struct style_node *)node)->class_name, name); return CSS_OK; }
static css_error has_id(void *pw, void *node, lwc_string *name, bool *match)
{ *match = matches(((struct style_node *)node)->id, name); return CSS_OK; }
static css_error no_attribute(void *pw, void *node, const css_qname *name, bool *match)
{ *match = false; return CSS_OK; }
static css_error no_attribute_value(void *pw, void *node, const css_qname *name, lwc_string *value, bool *match)
{ *match = false; return CSS_OK; }
static css_error root(void *pw, void *node, bool *match)
{ *match = ((struct style_node *)node)->parent == NULL; return CSS_OK; }
static css_error siblings(void *pw, void *node, bool same, bool after, int32_t *count)
{ *count = 0; return CSS_OK; }
static css_error no_state(void *pw, void *node, bool *match)
{ *match = false; return CSS_OK; }
static css_error no_lang(void *pw, void *node, lwc_string *lang, bool *match)
{ *match = false; return CSS_OK; }
static css_error no_hints(void *pw, void *node, uint32_t *count, css_hint **hints)
{ *count = 0; *hints = NULL; return CSS_OK; }
static css_error defaults(void *pw, uint32_t property, css_hint *hint)
{
    if (property == CSS_PROP_COLOR) { hint->data.color = 0xff000000UL; hint->status = CSS_COLOR_COLOR; }
    else if (property == CSS_PROP_FONT_FAMILY) { hint->data.strings = NULL; hint->status = CSS_FONT_FAMILY_SANS_SERIF; }
    else if (property == CSS_PROP_QUOTES) { hint->data.strings = NULL; hint->status = CSS_QUOTES_NONE; }
    else if (property == CSS_PROP_VOICE_FAMILY) { hint->data.strings = NULL; hint->status = 0; }
    else return CSS_INVALID;
    return CSS_OK;
}
static css_error set_cache(void *pw, void *node, void *data)
{ ((struct style_node *)node)->cache = data; return CSS_OK; }
static css_error get_cache(void *pw, void *node, void **data)
{ *data = ((struct style_node *)node)->cache; return CSS_OK; }

static css_select_handler style_handler = {
    .handler_version = CSS_SELECT_HANDLER_VERSION_1,
    .node_name = node_name, .node_classes = node_classes, .node_id = node_id,
    .named_ancestor_node = ancestor, .named_parent_node = named_parent,
    .named_sibling_node = no_named_sibling, .named_generic_sibling_node = no_named_sibling,
    .parent_node = parent, .sibling_node = no_sibling,
    .node_has_name = has_name, .node_has_class = has_class, .node_has_id = has_id,
    .node_has_attribute = no_attribute, .node_has_attribute_equal = no_attribute_value,
    .node_has_attribute_dashmatch = no_attribute_value, .node_has_attribute_includes = no_attribute_value,
    .node_has_attribute_prefix = no_attribute_value, .node_has_attribute_suffix = no_attribute_value,
    .node_has_attribute_substring = no_attribute_value,
    .node_is_root = root, .node_count_siblings = siblings, .node_is_empty = no_state,
    .node_is_link = no_state, .node_is_visited = no_state, .node_is_hover = no_state,
    .node_is_active = no_state, .node_is_focus = no_state, .node_is_enabled = no_state,
    .node_is_disabled = no_state, .node_is_checked = no_state, .node_is_target = no_state,
    .node_is_lang = no_lang, .node_presentational_hint = no_hints,
    .ua_default_for_property = defaults, .set_libcss_node_data = set_cache, .get_libcss_node_data = get_cache
};

static void styles(void)
{
    static const char rules[] =
        "p { color: #123456; width: 640px; padding: 96px; background-color: #283e4a; }"
        "#main { color: rgb(18, 52, 86); } .note { width: 320px; }"
        "body p { height: 480px; } p:hover { color: #abcdef; }";
    css_stylesheet_params parameters;
    css_stylesheet *sheet = NULL;
    css_select_ctx *context = NULL;
    css_select_results *result = NULL;
    css_unit_ctx units;
    css_media media;
    css_fixed length = 0;
    css_unit unit;
    css_color color = 0;
    struct style_node body = { "body", NULL, NULL, NULL, NULL };
    struct style_node paragraph = { "p", "main", "note", &body, NULL };
    size_t i;
    memset(&parameters, 0, sizeof(parameters));
    parameters.params_version = CSS_STYLESHEET_PARAMS_VERSION_1;
    parameters.level = CSS_LEVEL_21;
    parameters.charset = "UTF-8";
    parameters.url = "file:///test.html";
    parameters.resolve = resolve_url;
    CHECK(css_stylesheet_create(&parameters, &sheet) == CSS_OK);
    if (!sheet) return;
    for (i = 0; i < sizeof(rules) - 1; i++) {
        css_error error = css_stylesheet_append_data(sheet, (const ns_octet_t *)rules + i, 1);
        if (error != CSS_OK && error != CSS_NEEDDATA) break;
    }
    CHECK(i == sizeof(rules) - 1);
    CHECK(css_stylesheet_data_done(sheet) == CSS_OK);
    CHECK(css_select_ctx_create(&context) == CSS_OK);
    if (!context) { css_stylesheet_destroy(sheet); return; }
    CHECK(css_select_ctx_append_sheet(context, sheet, CSS_ORIGIN_AUTHOR, NULL) == CSS_OK);
    memset(&units, 0, sizeof(units));
    units.viewport_width = INTTOFIX(640); units.viewport_height = INTTOFIX(480);
    units.font_size_default = INTTOFIX(16); units.font_size_minimum = INTTOFIX(6);
    units.device_dpi = INTTOFIX(96);
    memset(&media, 0, sizeof(media)); media.type = CSS_MEDIA_SCREEN;
    CHECK(css_select_style(context, &paragraph, &units, &media, NULL, &style_handler, NULL, &result) == CSS_OK);
    if (result) {
        css_computed_style *style = result->styles[CSS_PSEUDO_ELEMENT_NONE];
        CHECK(css_computed_color(style, &color) == CSS_COLOR_COLOR && color == 0xff123456UL);
        CHECK(css_computed_background_color(style, &color) == CSS_BACKGROUND_COLOR_COLOR && color == 0xff283e4aUL);
        CHECK(css_computed_width(style, &length, &unit) == CSS_WIDTH_SET && length == INTTOFIX(320) && unit == CSS_UNIT_PX);
        CHECK(css_computed_height(style, &length, &unit) == CSS_HEIGHT_SET && length == INTTOFIX(480) && unit == CSS_UNIT_PX);
        CHECK(css_computed_padding_top(style, &length, &unit) == CSS_PADDING_SET && length == INTTOFIX(96) && unit == CSS_UNIT_PX);
        CHECK(css_select_results_destroy(result) == CSS_OK);
    }
    if (paragraph.cache) CHECK(css_libcss_node_data_handler(&style_handler, CSS_NODE_DELETED, NULL, &paragraph, NULL, paragraph.cache) == CSS_OK);
    if (body.cache) CHECK(css_libcss_node_data_handler(&style_handler, CSS_NODE_DELETED, NULL, &body, NULL, body.cache) == CSS_OK);
    CHECK(css_select_ctx_destroy(context) == CSS_OK);
    CHECK(css_stylesheet_destroy(sheet) == CSS_OK);
}
