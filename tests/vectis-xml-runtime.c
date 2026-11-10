#include <assert.h>
/* Vectis 6b1e263 test_vectis_xml_objects.c: original inputs and assertions,
 * executed through the installed-public-API receiver adapter. */
#include <stdio.h>
#undef assert
/* Keep the baseline expressions executable in release consumers. */
#define assert(test)                                                           \
    do {                                                                       \
        if (!(test)) {                                                         \
            fprintf(stderr, "XML adapter %d: %s\n", __LINE__, #test);          \
            abort();                                                           \
        }                                                                      \
    } while (0)
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include "vectis-xml.h"
static const char document[] =
    "<r xmlns='urn:r' xmlns:q='urn:q'><q:x a=''/><q:x a='2'>a&amp;b</q:x></r>";
static void native_dom(void)
{
    reference_xml_document *doc = NULL;
    reference_xml_node *root = NULL, *child = NULL, *old = NULL;
    reference_xml_document_config config;
    reference_xml_child_config child_config;
    reference_xml_namespace namespaces[1];
    reference_xml_xpath_result result;
    reference_xml_name name;
    struct reference_mutable_bytes bytes;
    reference_error error;
    int found;
    char *input = (char *)malloc(sizeof(document));
    assert(input != NULL);
    memcpy(input, document, sizeof(document));
    reference_xml_document_config_init(&config);
    assert(reference_xml_document_parse(input, sizeof(document) - 1u, &config,
                                        &doc, &error) == REFERENCE_OK);
    memset(input, 'x', sizeof(document));
    free(input);
    assert(doc->root(doc, &root, &error) == REFERENCE_OK);
    assert(root->name(root, &name, &error) == REFERENCE_OK &&
           strcmp(name.name, "r") == 0 &&
           strcmp(name.namespace_uri, "urn:r") == 0 && name.prefix == NULL);
    namespaces[0].prefix = "q";
    namespaces[0].uri = "urn:q";
    assert(doc->xpath(doc, "//q:x", namespaces, 1u, &result, &error) ==
               REFERENCE_OK &&
           result.count == 2u);
    assert(result.nodes[0]->attribute(result.nodes[0], "a", NULL, &bytes,
                                      &found, &error) == REFERENCE_OK &&
           found && bytes.size == 0u);
    reference_mutable_bytes_cleanup(&bytes);
    assert(result.nodes[1]->text(result.nodes[1], &bytes, &error) ==
               REFERENCE_OK &&
           bytes.size == 3u && memcmp(bytes.data, "a&b", 3u) == 0);
    reference_mutable_bytes_cleanup(&bytes);
    old = result.nodes[0];
    result.nodes[0] = NULL;
    reference_xml_xpath_result_cleanup(&result);
    assert(doc->xpath(doc, "count(//q:x)", namespaces, 1u, &result, &error) ==
               REFERENCE_OK &&
           result.kind == REFERENCE_XML_XPATH_NUMBER && result.number == 2.0);
    reference_xml_xpath_result_cleanup(&result);
    assert(doc->xpath(doc, "boolean(//q:x)", namespaces, 1u, &result, &error) ==
               REFERENCE_OK &&
           result.boolean);
    reference_xml_xpath_result_cleanup(&result);
    assert(doc->xpath(doc, "string(//q:x[2]/@a)", namespaces, 1u, &result,
                      &error) == REFERENCE_OK &&
           result.string.size == 1u);
    reference_xml_xpath_result_cleanup(&result);
    assert(doc->xpath(doc, "//q:x/@a", namespaces, 1u, &result, &error) ==
           REFERENCE_INVALID);
    assert(doc->xpath(doc, "[", NULL, 0u, &result, &error) != REFERENCE_OK);
    assert(root->set_attribute(root, "x", "<&", 2u, "urn:q", "q", &error) ==
           REFERENCE_OK);
    assert(old->name(old, &name, &error) == REFERENCE_STATE);
    assert(old->close(old, &error) == REFERENCE_OK);
    assert(root->name(root, &name, &error) == REFERENCE_STATE);
    assert(root->close(root, &error) == REFERENCE_OK);
    assert(doc->root(doc, &root, &error) == REFERENCE_OK);
    assert(root->attribute(root, "x", "urn:q", &bytes, &found, &error) ==
               REFERENCE_OK &&
           found && bytes.size == 2u && memcmp(bytes.data, "<&", 2u) == 0);
    reference_mutable_bytes_cleanup(&bytes);
    assert(root->set_attribute(root, "x", "v", 1u, "urn:other", "q", &error) ==
           REFERENCE_CONFLICT);
    assert(root->set_attribute(root, "x", "v", 1u, "urn:other", "xml",
                               &error) == REFERENCE_INVALID);
    assert(root->set_text(root, "\0", 1u, &error) == REFERENCE_INVALID);
    memset(&child_config, 0, sizeof(child_config));
    child_config.text = "<&";
    child_config.text_size = 2u;
    child_config.text_present = 1;
    assert(root->append_child(root, "item", &child_config, &child, &error) ==
           REFERENCE_OK);
    assert(child->name(child, &name, &error) == REFERENCE_OK &&
           strcmp(name.namespace_uri, "urn:r") == 0);
    assert(root->close(root, &error) == REFERENCE_OK);
    assert(child->set_text(child, "literal &value;",
                           sizeof("literal &value;") - 1u,
                           &error) == REFERENCE_OK);
    assert(child->close(child, &error) == REFERENCE_OK);
    assert(doc->serialize_materialized(doc, 1, &bytes, &error) == REFERENCE_OK);
    assert(bytes.size > 0u);
    reference_mutable_bytes_cleanup(&bytes);
    assert(doc->root(doc, &root, &error) == REFERENCE_OK);
    assert(root->remove(root, &error) == REFERENCE_INVALID);
    assert(doc->close(doc, &error) == REFERENCE_OK);
    assert(root->name(root, &name, &error) == REFERENCE_STATE);
    assert(root->close(root, &error) == REFERENCE_OK);
    assert(reference_xml_document_parse("<r>", 3u, NULL, &doc, &error) !=
               REFERENCE_OK &&
           doc == NULL);
    assert(reference_xml_document_parse(
               "<!DOCTYPE r [<!ENTITY a 'x'>]><r>&a;</r>",
               sizeof("<!DOCTYPE r [<!ENTITY a 'x'>]><r>&a;</r>") - 1u, NULL,
               &doc, &error) != REFERENCE_OK &&
           doc == NULL);
    config.max_bytes = 2u;
    assert(reference_xml_document_parse("<r/>", 4u, &config, &doc, &error) ==
           REFERENCE_INVALID);
    reference_xml_document_config_init(&config);
    config.max_xpath_nodes = 1u;
    assert(reference_xml_document_parse(document, sizeof(document) - 1u,
                                        &config, &doc, &error) == REFERENCE_OK);
    assert(doc->xpath(doc, "//*", NULL, 0u, &result, &error) ==
           REFERENCE_INVALID);
    assert(doc->close(doc, &error) == REFERENCE_OK);
}
typedef struct events {
    reference_xml_sax *parser;
    size_t starts, ends, bytes;
    int fail, reenter;
} events;
static reference_status start(void *context, const reference_xml_name *name,
                              const reference_xml_attribute *attrs,
                              size_t count, const reference_xml_namespace *ns,
                              size_t ns_count, reference_error *error)
{
    events *state = (events *)context;
    (void)error;
    ++state->starts;
    assert(name->name != NULL);
    if (state->starts == 1u && ns_count != 0u) {
        assert(strcmp(ns[0].uri, "urn:r") == 0);
        assert(count == 1u && attrs[0].size == 1u &&
               memcmp(attrs[0].value, "&", 1u) == 0);
    }
    if (state->reenter) {
        assert(state->parser->close(state->parser, error) == REFERENCE_STATE);
        return REFERENCE_OK;
    }
    return state->fail ? REFERENCE_STATE : REFERENCE_OK;
}
static reference_status end(void *context, const reference_xml_name *name,
                            reference_error *error)
{
    events *state = (events *)context;
    (void)name;
    (void)error;
    ++state->ends;
    return REFERENCE_OK;
}
static reference_status text(void *context, const void *data, size_t size,
                             reference_error *error)
{
    events *state = (events *)context;
    (void)error;
    assert(data != NULL && size <= REFERENCE_XML_SAX_CHUNK_BYTES);
    state->bytes += size;
    return REFERENCE_OK;
}
static void native_sax(void)
{
    reference_xml_sax_callbacks callbacks;
    events state;
    reference_error error;
    char chunk[4096];
    size_t i;
    memset(&state, 0, sizeof(state));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.start_element = start;
    callbacks.end_element = end;
    callbacks.text = text;
    assert(reference_xml_sax_new(&callbacks, &state, 0u, &state.parser,
                                 &error) == REFERENCE_OK);
    assert(state.parser->feed(state.parser, "<r xmlns='urn:r' a='&amp;'>",
                              sizeof("<r xmlns='urn:r' a='&amp;'>") - 1u,
                              &error) == REFERENCE_OK);
    assert(state.starts == 1u && state.ends == 0u);
    memset(chunk, 'x', sizeof(chunk));
    for (i = 0u; i < 4097u; ++i)
        assert(state.parser->feed(state.parser, chunk, sizeof(chunk), &error) ==
               REFERENCE_OK);
    assert(state.bytes > REFERENCE_XML_DEFAULT_MAX_BYTES);
    assert(state.parser->feed(state.parser, "</r>", 4u, &error) ==
           REFERENCE_OK);
    assert(state.parser->finish(state.parser, &error) == REFERENCE_OK &&
           state.ends == 1u);
    assert(state.parser->finish(state.parser, &error) == REFERENCE_OK);
    assert(state.parser->feed(state.parser, "", 0u, &error) == REFERENCE_STATE);
    assert(state.parser->close(state.parser, &error) == REFERENCE_OK);
    memset(&state, 0, sizeof(state));
    state.fail = 1;
    assert(reference_xml_sax_new(&callbacks, &state, 0u, &state.parser,
                                 &error) == REFERENCE_OK);
    assert(state.parser->feed(state.parser, "<r/>", 4u, &error) ==
           REFERENCE_STATE);
    assert(state.parser->finish(state.parser, &error) == REFERENCE_STATE &&
           state.starts == 1u);
    assert(state.parser->close(state.parser, &error) == REFERENCE_OK);
    memset(&state, 0, sizeof(state));
    state.reenter = 1;
    assert(reference_xml_sax_new(&callbacks, &state, 0u, &state.parser,
                                 &error) == REFERENCE_OK);
    assert(state.parser->feed(state.parser, "<r/>", 4u, &error) ==
           REFERENCE_STATE);
    assert(state.parser->close(state.parser, &error) == REFERENCE_OK);
    memset(&state, 0, sizeof(state));
    assert(reference_xml_sax_new(&callbacks, &state, 0u, &state.parser,
                                 &error) == REFERENCE_OK);
    assert(state.parser->feed(
               state.parser, "<!DOCTYPE r SYSTEM 'file:///unused'><r/>",
               sizeof("<!DOCTYPE r SYSTEM 'file:///unused'><r/>") - 1u,
               &error) != REFERENCE_OK);
    assert(state.starts == 0u &&
           state.parser->close(state.parser, &error) == REFERENCE_OK);
    assert(reference_xml_sax_new(&callbacks, &state, 1u, &state.parser,
                                 &error) == REFERENCE_OK);
    assert(state.parser->feed(state.parser, "<root/>", 7u, &error) ==
           REFERENCE_INVALID);
    assert(state.parser->close(state.parser, &error) == REFERENCE_OK);
    assert(reference_xml_sax_new(&callbacks, &state, 0u, &state.parser,
                                 &error) == REFERENCE_OK);
    assert(state.parser->feed(state.parser, "<r>", 3u, &error) == REFERENCE_OK);
    assert(state.parser->finish(state.parser, &error) != REFERENCE_OK);
    assert(state.parser->close(state.parser, &error) == REFERENCE_OK);
}
static void *foreign_thread(void *context)
{
    reference_xml_document *doc = (reference_xml_document *)context;
    reference_xml_node *node = NULL;
    reference_error error;
    assert(doc->root(doc, &node, &error) == REFERENCE_STATE && node == NULL);
    assert(doc->close(doc, &error) == REFERENCE_STATE);
    return NULL;
}
static void ownership(void)
{
    reference_xml_document *doc = NULL;
    reference_xml_node *node = NULL;
    reference_error error;
    pthread_t thread;
    pid_t child;
    int status;
    assert(reference_xml_document_parse("<r/>", 4u, NULL, &doc, &error) ==
           REFERENCE_OK);
    assert(pthread_create(&thread, NULL, foreign_thread, doc) == 0);
    assert(pthread_join(thread, NULL) == 0);
    child = fork();
    assert(child >= 0);
    if (child == 0) {
        foreign_thread(doc);
        _exit(0);
    }
    assert(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
           WEXITSTATUS(status) == 0);
    assert(doc->root(doc, &node, &error) == REFERENCE_OK);
    assert(node->close(node, &error) == REFERENCE_OK);
    assert(doc->close(doc, &error) == REFERENCE_OK);
}
int main(void)
{
    native_dom();
    native_sax();
    ownership();
    return 0;
}
