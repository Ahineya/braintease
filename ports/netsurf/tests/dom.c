/* Actual LibDOM/Hubbub integration, shared by the host and both guests. */
#include <dom/dom.h>
#include <dom/html/html_document.h>
#include <dom/html/html_collection.h>
#include <dom/html/html_input_element.h>
#include <dom/bindings/hubbub/parser.h>

static dom_string *dom_word(const char *text)
{
    dom_string *word = NULL;
    CHECK(dom_string_create((const ns_octet_t *)text, strlen(text), &word) == DOM_NO_ERR);
    return word;
}

static bool dom_text_is(dom_string *value, const char *expected)
{
    return value && dom_string_byte_length(value) == strlen(expected) &&
        !memcmp(dom_string_data(value), expected, strlen(expected));
}

static dom_element *dom_id(dom_document *document, const char *id)
{
    dom_string *key = dom_word(id);
    dom_element *element = NULL;
    CHECK(key && dom_document_get_element_by_id(document, key, &element) == DOM_NO_ERR);
    dom_string_unref(key);
    return element;
}

static void dom_strings(void)
{
    dom_string *value = dom_word("AЖ🙂Z"), *part = NULL, *changed = NULL;
    dom_string *replacement = dom_word("abc");
    uint32_t scalar = 0;
    CHECK(value && dom_string_length(value) == 4 && dom_string_byte_length(value) == 8);
    CHECK(dom_string_at(value, 2, &scalar) == DOM_NO_ERR && scalar == 0x1f642UL);
    CHECK(dom_string_substr(value, 1, 3, &part) == DOM_NO_ERR && dom_text_is(part, "Ж🙂"));
    CHECK(dom_string_replace(value, replacement, 1, 3, &changed) == DOM_NO_ERR && dom_text_is(changed, "AabcZ"));
    dom_string_unref(changed); dom_string_unref(part);
    dom_string_unref(value); dom_string_unref(replacement);
    CHECK(DOM_NO_MEM_ERR == 131072UL && DOM_UNSPECIFIED_EVENT_TYPE_ERR == 65536UL);
    CHECK(DOM_HUBBUB_HUBBUB_ERR_BADENCODING == (65536UL | HUBBUB_BADENCODING));
    {
        char *large = malloc(70001UL);
        CHECK(large != NULL);
        if (!large) return;
        memset(large, 'x', 70000UL); large[70000UL] = 0;
        large[63999UL] = 'b'; large[64000UL] = 'c';
        value = NULL; part = NULL;
        CHECK(dom_string_create((const ns_octet_t *)large, 70000UL, &value) == DOM_NO_ERR);
        if (value) {
            CHECK(dom_string_length(value) == 70000UL);
            CHECK(dom_string_substr(value, 63998UL, 64002UL, &part) == DOM_NO_ERR && dom_text_is(part, "xbcx"));
        }
        dom_string_unref(part); dom_string_unref(value); free(large);
    }
}

static void dom_tree(void)
{
    static const char input[] = "<!doctype html><title> C16 &amp; DOM </title>"
        "<body><div id='main' class='page bright'><p id='greeting'>Ж &#x1f642;</p>"
        "<table id='grid'><tr><td>A<td>B</table><a href='next.html'>Next</a>"
        "<form><input id='query' name='q' value='hello'></form></div>";
    dom_hubbub_parser_params params;
    dom_hubbub_parser *parser = NULL;
    dom_document *document = NULL;
    dom_element *main = NULL, *greeting = NULL, *table = NULL, *input_element = NULL;
    dom_nodelist *cells = NULL;
    dom_html_collection *links = NULL;
    dom_node *node = NULL, *parent = NULL, *clone = NULL, *removed = NULL;
    dom_string *value = NULL, *key = NULL;
    dom_document_quirks_mode quirks;
    uint32_t length = 0;
    int32_t signed_value = 0;
    lwc_string *class_name = NULL;
    bool match = false;
    size_t i;
    memset(&params, 0, sizeof(params));
    params.enc = "UTF-8"; params.fix_enc = true;
    /* Script execution remains disabled in the initial browser profile. */
    CHECK(dom_hubbub_parser_create(&params, &parser, &document) == DOM_HUBBUB_OK);
    if (!parser || !document) return;
    for (i = 0; i < sizeof(input) - 1; i++)
        if (dom_hubbub_parser_parse_chunk(parser, (const ns_octet_t *)input + i, 1) != DOM_HUBBUB_OK) break;
    CHECK(i == sizeof(input) - 1);
    CHECK(dom_hubbub_parser_completed(parser) == DOM_HUBBUB_OK);
    dom_hubbub_parser_destroy(parser);
    /* The caller owns the document after parser destruction. */
    CHECK(dom_document_get_quirks_mode(document, &quirks) == DOM_NO_ERR && quirks == DOM_DOCUMENT_QUIRKS_MODE_NONE);
    /* LibDOM 0.4.2 returns raw title text; frontend display normalizes it. */
    CHECK(dom_html_document_get_title(document, &value) == DOM_NO_ERR && dom_text_is(value, " C16 & DOM "));
    dom_string_unref(value); value = NULL;
    main = dom_id(document, "main"); greeting = dom_id(document, "greeting"); table = dom_id(document, "grid");
    CHECK(main && greeting && table);
    if (!main || !greeting || !table) goto done;
    CHECK(dom_node_get_text_content(greeting, &value) == DOM_NO_ERR && dom_text_is(value, "Ж 🙂"));
    CHECK(value && dom_string_length(value) == 3);
    dom_string_unref(value); value = NULL;
    CHECK(dom_node_get_parent_node(greeting, &parent) == DOM_NO_ERR && parent == (dom_node *)main);
    dom_node_unref(parent); parent = NULL;
    CHECK(lwc_intern_string("bright", 6, &class_name) == lwc_error_ok);
    CHECK(dom_element_has_class(main, class_name, &match) == DOM_NO_ERR && match);
    lwc_string_unref(class_name);
    key = dom_word("td");
    CHECK(dom_element_get_elements_by_tag_name(table, key, &cells) == DOM_NO_ERR);
    dom_string_unref(key); key = NULL;
    CHECK(cells && dom_nodelist_get_length(cells, &length) == DOM_NO_ERR && length == 2);
    CHECK(dom_nodelist_item(cells, 1, &node) == DOM_NO_ERR && node);
    CHECK(node && dom_node_get_text_content(node, &value) == DOM_NO_ERR && dom_text_is(value, "B"));
    dom_string_unref(value); value = NULL; dom_node_unref(node); node = NULL;
    /* HTML tree construction inserts the omitted tbody and closes both td's. */
    key = dom_word("tbody");
    dom_nodelist_unref(cells); cells = NULL;
    CHECK(dom_element_get_elements_by_tag_name(table, key, &cells) == DOM_NO_ERR);
    dom_string_unref(key); key = NULL;
    CHECK(dom_nodelist_get_length(cells, &length) == DOM_NO_ERR && length == 1);
    CHECK(dom_html_document_get_links(document, &links) == DOM_NO_ERR);
    CHECK(dom_html_collection_get_length(links, &length) == DOM_NO_ERR && length == 1);
    CHECK(dom_html_collection_item(links, 0, &node) == DOM_NO_ERR && node);
    key = dom_word("href");
    CHECK(dom_element_get_attribute(node, key, &value) == DOM_NO_ERR && dom_text_is(value, "next.html"));
    dom_string_unref(key); key = NULL; dom_string_unref(value); value = NULL;
    dom_node_unref(node); node = NULL;
    input_element = dom_id(document, "query");
    CHECK(input_element != NULL);
    if (input_element) {
        CHECK(dom_html_input_element_get_value((dom_html_input_element *)input_element, &value) == DOM_NO_ERR && dom_text_is(value, "hello"));
        dom_string_unref(value); value = NULL;
        CHECK(dom_html_input_element_set_size((dom_html_input_element *)input_element, 70000UL) == DOM_NO_ERR);
        CHECK(dom_html_input_element_get_size((dom_html_input_element *)input_element, &length) == DOM_NO_ERR && length == 70000UL);
        CHECK(dom_html_input_element_set_max_length((dom_html_input_element *)input_element, 90000UL) == DOM_NO_ERR);
        CHECK(dom_html_input_element_get_max_length((dom_html_input_element *)input_element, &signed_value) == DOM_NO_ERR && signed_value == 90000L);
    }
    /* Mutation and cloning exercise the retained tree's ownership rules. */
    CHECK(dom_node_clone_node(greeting, true, &clone) == DOM_NO_ERR && clone);
    CHECK(clone && dom_node_get_text_content(clone, &value) == DOM_NO_ERR && dom_text_is(value, "Ж 🙂"));
    dom_string_unref(value); value = dom_word("Changed");
    CHECK(dom_node_set_text_content(greeting, value) == DOM_NO_ERR);
    dom_string_unref(value); value = NULL;
    CHECK(dom_node_get_text_content(clone, &value) == DOM_NO_ERR && dom_text_is(value, "Ж 🙂"));
    dom_string_unref(value); value = NULL;
    CHECK(dom_node_remove_child(main, greeting, &removed) == DOM_NO_ERR && removed == (dom_node *)greeting);
    dom_node_unref(removed); removed = NULL;
    CHECK(dom_node_get_parent_node(greeting, &parent) == DOM_NO_ERR && parent == NULL);
    CHECK(dom_node_append_child(main, greeting, &node) == DOM_NO_ERR && node == (dom_node *)greeting);
    dom_node_unref(node); node = NULL;
    CHECK(dom_node_append_child(greeting, main, &node) == DOM_HIERARCHY_REQUEST_ERR);
done:
    dom_string_unref(key); dom_string_unref(value);
    if (links) dom_html_collection_unref(links);
    if (cells) dom_nodelist_unref(cells);
    dom_node_unref(clone); dom_node_unref(node); dom_node_unref(parent);
    dom_node_unref(input_element);
    dom_node_unref(table); dom_node_unref(greeting); dom_node_unref(main); dom_node_unref(document);
}

static void dom_large_text(void)
{
    static const char start[] = "<!doctype html><p id='large'>";
    static const char end[] = "</p>";
    dom_hubbub_parser_params params;
    dom_hubbub_parser *parser = NULL;
    dom_document *document = NULL;
    dom_element *element = NULL;
    dom_string *text = NULL, *part = NULL;
    ns_octet_t buffer[512];
    size_t offset, count;
    memset(&params, 0, sizeof(params));
    params.enc = "UTF-8"; params.fix_enc = true;
    CHECK(dom_hubbub_parser_create(&params, &parser, &document) == DOM_HUBBUB_OK);
    if (!parser || !document) return;
    CHECK(dom_hubbub_parser_parse_chunk(parser, (const ns_octet_t *)start, sizeof(start) - 1) == DOM_HUBBUB_OK);
    /* Text-node appends and byte lengths must survive the int16 boundary,
     * not merely the standalone string APIs. No large input buffer is held. */
    for (offset = 0; offset < 70000UL; offset += count) {
        count = 70000UL - offset;
        if (count > sizeof(buffer)) count = sizeof(buffer);
        memset(buffer, 'x', count);
        if (offset <= 63999UL && offset + count > 63999UL) buffer[63999UL - offset] = 'b';
        if (offset <= 64000UL && offset + count > 64000UL) buffer[64000UL - offset] = 'c';
        if (dom_hubbub_parser_parse_chunk(parser, buffer, count) != DOM_HUBBUB_OK) break;
    }
    CHECK(offset == 70000UL);
    CHECK(dom_hubbub_parser_parse_chunk(parser, (const ns_octet_t *)end, sizeof(end) - 1) == DOM_HUBBUB_OK);
    CHECK(dom_hubbub_parser_completed(parser) == DOM_HUBBUB_OK);
    dom_hubbub_parser_destroy(parser);
    element = dom_id(document, "large");
    CHECK(element && dom_node_get_text_content(element, &text) == DOM_NO_ERR);
    if (text) {
        CHECK(dom_string_length(text) == 70000UL && dom_string_byte_length(text) == 70000UL);
        CHECK(dom_string_substr(text, 63998UL, 64002UL, &part) == DOM_NO_ERR && dom_text_is(part, "xbcx"));
    }
    dom_string_unref(part); dom_string_unref(text); dom_node_unref(element); dom_node_unref(document);
}

static void dom_tests(void)
{
    unsigned cycle;
    dom_strings();
    /* Creation/destruction must also work repeatedly in one guest process. */
    for (cycle = 0; cycle < 4; cycle++) dom_tree();
    dom_large_text();
}
