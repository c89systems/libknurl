#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            (void)fprintf(stderr, "XML %d: %s\n", __LINE__, #t);               \
            abort();                                                           \
        }                                                                      \
    } while (0)
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <knurl/xml.h>
static const char document[] =
    "<r xmlns='urn:r' xmlns:q='urn:q'><q:x a=''/><q:x a='2'>a&amp;b</q:x></r>";
static void native_dom(void)
{
    knurl_xml_document *doc = NULL;
    knurl_xml_node *root = NULL, *child = NULL, *old = NULL;
    knurl_xml_document_config config;
    knurl_xml_child_config child_config;
    knurl_xml_namespace namespaces[1];
    knurl_xml_xpath_result result;
    knurl_xml_name name;
    knurl_owned_bytes bytes;
    knurl_error error;
    int found;
    char *input = (char *)malloc(sizeof(document));
    CHECK(input != NULL);
    memcpy(input, document, sizeof(document));
    knurl_xml_document_config_init(&config);
    CHECK(knurl_xml_document_parse(input, sizeof(document) - 1u, &config, &doc,
                                   &error) == KNURL_OK);
    memset(input, 'x', sizeof(document));
    free(input);
    CHECK(doc->root(doc, &root, &error) == KNURL_OK);
    CHECK(root->name(root, &name, &error) == KNURL_OK &&
          strcmp(name.name, "r") == 0 &&
          strcmp(name.namespace_uri, "urn:r") == 0 && name.prefix == NULL);
    namespaces[0].prefix = "q";
    namespaces[0].uri = "urn:q";
    CHECK(doc->xpath(doc, "//q:x", namespaces, 1u, &result, &error) ==
              KNURL_OK &&
          result.count == 2u);
    CHECK(knurl_xml_node_attribute(result.nodes[0], "a", NULL, &bytes, &found,
                                   &error) == KNURL_OK &&
          found && bytes.length == 0u);
    knurl_owned_bytes_cleanup(&bytes);
    CHECK(knurl_xml_node_text(result.nodes[1], &bytes, &error) == KNURL_OK &&
          bytes.length == 3u && memcmp(bytes.data, "a&b", 3u) == 0);
    knurl_owned_bytes_cleanup(&bytes);
    old = result.nodes[0];
    result.nodes[0] = NULL;
    knurl_xml_xpath_result_cleanup(&result);
    CHECK(doc->xpath(doc, "count(//q:x)", namespaces, 1u, &result, &error) ==
              KNURL_OK &&
          result.kind == KNURL_XML_XPATH_NUMBER && result.number == 2.0);
    knurl_xml_xpath_result_cleanup(&result);
    CHECK(doc->xpath(doc, "boolean(//q:x)", namespaces, 1u, &result, &error) ==
              KNURL_OK &&
          result.boolean);
    knurl_xml_xpath_result_cleanup(&result);
    CHECK(doc->xpath(doc, "string(//q:x[2]/@a)", namespaces, 1u, &result,
                     &error) == KNURL_OK &&
          result.string.length == 1u);
    knurl_xml_xpath_result_cleanup(&result);
    CHECK(doc->xpath(doc, "//q:x/@a", namespaces, 1u, &result, &error) ==
          KNURL_INVALID);
    CHECK(doc->xpath(doc, "[", NULL, 0u, &result, &error) != KNURL_OK);
    CHECK(root->set_attribute(root, "x", "<&", 2u, "urn:q", "q", &error) ==
          KNURL_OK);
    CHECK(old->name(old, &name, &error) == KNURL_STATE);
    CHECK(old->close(old, &error) == KNURL_OK);
    CHECK(root->name(root, &name, &error) == KNURL_STATE);
    CHECK(root->close(root, &error) == KNURL_OK);
    CHECK(doc->root(doc, &root, &error) == KNURL_OK);
    CHECK(root->attribute(root, "x", "urn:q", &bytes, &found, &error) ==
              KNURL_OK &&
          found && bytes.length == 2u && memcmp(bytes.data, "<&", 2u) == 0);
    knurl_owned_bytes_cleanup(&bytes);
    CHECK(root->set_attribute(root, "x", "v", 1u, "urn:other", "q", &error) ==
          KNURL_CONFLICT);
    CHECK(root->set_attribute(root, "x", "v", 1u, "urn:other", "xml", &error) ==
          KNURL_INVALID);
    CHECK(root->set_text(root, "\0", 1u, &error) == KNURL_INVALID);
    memset(&child_config, 0, sizeof(child_config));
    child_config.text = "<&";
    child_config.text_size = 2u;
    child_config.text_present = 1;
    CHECK(root->append_child(root, "item", &child_config, &child, &error) ==
          KNURL_OK);
    CHECK(child->name(child, &name, &error) == KNURL_OK &&
          strcmp(name.namespace_uri, "urn:r") == 0);
    CHECK(root->close(root, &error) == KNURL_OK);
    CHECK(child->set_text(child, "literal &value;",
                          sizeof("literal &value;") - 1u, &error) == KNURL_OK);
    CHECK(child->close(child, &error) == KNURL_OK);
    CHECK(doc->serialize_materialized(doc, 1, &bytes, &error) == KNURL_OK);
    CHECK(bytes.length > 0u);
    knurl_owned_bytes_cleanup(&bytes);
    CHECK(doc->root(doc, &root, &error) == KNURL_OK);
    CHECK(root->remove(root, &error) == KNURL_INVALID);
    CHECK(doc->close(doc, &error) == KNURL_OK);
    CHECK(root->name(root, &name, &error) == KNURL_STATE);
    CHECK(root->close(root, &error) == KNURL_OK);
    CHECK(knurl_xml_document_parse("<r>", 3u, NULL, &doc, &error) != KNURL_OK &&
          doc == NULL);
    CHECK(knurl_xml_document_parse(
              "<!DOCTYPE r [<!ENTITY a 'x'>]><r>&a;</r>",
              sizeof("<!DOCTYPE r [<!ENTITY a 'x'>]><r>&a;</r>") - 1u, NULL,
              &doc, &error) != KNURL_OK &&
          doc == NULL);
    config.max_bytes = 2u;
    CHECK(knurl_xml_document_parse("<r/>", 4u, &config, &doc, &error) ==
          KNURL_INVALID);
    knurl_xml_document_config_init(&config);
    config.max_xpath_nodes = 1u;
    CHECK(knurl_xml_document_parse(document, sizeof(document) - 1u, &config,
                                   &doc, &error) == KNURL_OK);
    CHECK(doc->xpath(doc, "//*", NULL, 0u, &result, &error) == KNURL_INVALID);
    CHECK(doc->close(doc, &error) == KNURL_OK);
}
typedef struct events {
    knurl_xml_sax *parser;
    size_t starts, ends, bytes;
    int fail, reenter;
} events;
static knurl_status start(void *context, const knurl_xml_name *name,
                          const knurl_xml_attribute *attrs, size_t count,
                          const knurl_xml_namespace *ns, size_t ns_count,
                          knurl_error *error)
{
    events *state = (events *)context;
    (void)error;
    ++state->starts;
    CHECK(name->name != NULL);
    if (state->starts == 1u && ns_count != 0u) {
        CHECK(strcmp(ns[0].uri, "urn:r") == 0);
        CHECK(count == 1u && attrs[0].length == 1u &&
              memcmp(attrs[0].value, "&", 1u) == 0);
    }
    if (state->reenter) {
        CHECK(state->parser->close(state->parser, error) == KNURL_STATE);
        return KNURL_OK;
    }
    return state->fail == 2 ? KNURL_AGAIN
                            : (state->fail ? KNURL_STATE : KNURL_OK);
}
static knurl_status end(void *context, const knurl_xml_name *name,
                        knurl_error *error)
{
    events *state = (events *)context;
    (void)name;
    (void)error;
    ++state->ends;
    return KNURL_OK;
}
static knurl_status text(void *context, const void *data, size_t size,
                         knurl_error *error)
{
    events *state = (events *)context;
    (void)error;
    CHECK(data != NULL && size <= KNURL_XML_SAX_CHUNK_BYTES);
    state->bytes += size;
    return KNURL_OK;
}
static void native_sax(void)
{
    knurl_xml_sax_callbacks callbacks;
    events state;
    knurl_error error;
    char chunk[4096];
    size_t i;
    memset(&state, 0, sizeof(state));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.start_element = start;
    callbacks.end_element = end;
    callbacks.text = text;
    CHECK(knurl_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          KNURL_OK);
    CHECK(knurl_xml_sax_feed(state.parser, "<r xmlns='urn:r' a='&amp;'>",
                             sizeof("<r xmlns='urn:r' a='&amp;'>") - 1u,
                             &error) == KNURL_OK);
    CHECK(state.starts == 1u && state.ends == 0u);
    memset(chunk, 'x', sizeof(chunk));
    for (i = 0u; i < 4097u; ++i)
        CHECK(knurl_xml_sax_feed(state.parser, chunk, sizeof(chunk), &error) ==
              KNURL_OK);
    CHECK(state.bytes > KNURL_XML_DEFAULT_MAX_BYTES);
    CHECK(knurl_xml_sax_feed(state.parser, "</r>", 4u, &error) == KNURL_OK);
    CHECK(knurl_xml_sax_finish(state.parser, &error) == KNURL_OK &&
          state.ends == 1u);
    CHECK(knurl_xml_sax_finish(state.parser, &error) == KNURL_OK);
    CHECK(knurl_xml_sax_feed(state.parser, "", 0u, &error) == KNURL_STATE);
    CHECK(knurl_xml_sax_close(state.parser, &error) == KNURL_OK);
    memset(&state, 0, sizeof(state));
    state.fail = 1;
    CHECK(knurl_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          KNURL_OK);
    CHECK(knurl_xml_sax_feed(state.parser, "<r/>", 4u, &error) == KNURL_STATE);
    CHECK(knurl_xml_sax_finish(state.parser, &error) == KNURL_STATE &&
          state.starts == 1u);
    CHECK(knurl_xml_sax_close(state.parser, &error) == KNURL_OK);
    memset(&state, 0, sizeof(state));
    state.fail = 2;
    CHECK(knurl_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          KNURL_OK);
    CHECK(knurl_xml_sax_feed(state.parser, "<r/>", 4u, &error) ==
          KNURL_INVALID);
    CHECK(knurl_xml_sax_finish(state.parser, &error) == KNURL_INVALID &&
          state.starts == 1u);
    CHECK(knurl_xml_sax_close(state.parser, &error) == KNURL_OK);
    memset(&state, 0, sizeof(state));
    state.reenter = 1;
    CHECK(knurl_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          KNURL_OK);
    CHECK(knurl_xml_sax_feed(state.parser, "<r/>", 4u, &error) == KNURL_STATE);
    CHECK(knurl_xml_sax_close(state.parser, &error) == KNURL_OK);
    memset(&state, 0, sizeof(state));
    CHECK(knurl_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          KNURL_OK);
    CHECK(knurl_xml_sax_feed(
              state.parser, "<!DOCTYPE r SYSTEM 'file:///unused'><r/>",
              sizeof("<!DOCTYPE r SYSTEM 'file:///unused'><r/>") - 1u,
              &error) != KNURL_OK);
    CHECK(state.starts == 0u &&
          knurl_xml_sax_close(state.parser, &error) == KNURL_OK);
    CHECK(knurl_xml_sax_new(&callbacks, &state, 1u, &state.parser, &error) ==
          KNURL_OK);
    CHECK(knurl_xml_sax_feed(state.parser, "<root/>", 7u, &error) ==
          KNURL_INVALID);
    CHECK(knurl_xml_sax_close(state.parser, &error) == KNURL_OK);
    CHECK(knurl_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          KNURL_OK);
    CHECK(knurl_xml_sax_feed(state.parser, "<r>", 3u, &error) == KNURL_OK);
    CHECK(knurl_xml_sax_finish(state.parser, &error) != KNURL_OK);
    CHECK(knurl_xml_sax_close(state.parser, &error) == KNURL_OK);
}
static void *foreign_thread(void *context)
{
    knurl_xml_document *doc = (knurl_xml_document *)context;
    knurl_xml_node *node = NULL;
    knurl_error error;
    CHECK(doc->root(doc, &node, &error) == KNURL_STATE && node == NULL);
    CHECK(doc->close(doc, &error) == KNURL_STATE);
    return NULL;
}
static void ownership(void)
{
    knurl_xml_document *doc = NULL;
    knurl_xml_node *node = NULL;
    knurl_error error;
    pthread_t thread;
    pid_t child;
    int status;
    CHECK(knurl_xml_document_parse("<r/>", 4u, NULL, &doc, &error) == KNURL_OK);
    CHECK(pthread_create(&thread, NULL, foreign_thread, doc) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    child = fork();
    CHECK(child >= 0);
    if (child == 0) {
        foreign_thread(doc);
        _exit(0);
    }
    CHECK(waitpid(child, &status, 0) == child && WIFEXITED(status) &&
          WEXITSTATUS(status) == 0);
    CHECK(doc->root(doc, &node, &error) == KNURL_OK);
    CHECK(node->close(node, &error) == KNURL_OK);
    CHECK(doc->close(doc, &error) == KNURL_OK);
}
static void *worker(void *unused)
{
    (void)unused;
    native_dom();
    native_sax();
    return NULL;
}
int main(void)
{
    pthread_t thread;
    ownership();
    CHECK(pthread_create(&thread, NULL, worker, NULL) == 0);
    CHECK(pthread_join(thread, NULL) == 0);
    return 0;
}
