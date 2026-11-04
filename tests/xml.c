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
#include <rotmos/xml.h>
static const char document[] =
    "<r xmlns='urn:r' xmlns:q='urn:q'><q:x a=''/><q:x a='2'>a&amp;b</q:x></r>";
static void native_dom(void)
{
    rotmos_xml_document *doc = NULL;
    rotmos_xml_node *root = NULL, *child = NULL, *old = NULL;
    rotmos_xml_document_config config;
    rotmos_xml_child_config child_config;
    rotmos_xml_namespace namespaces[1];
    rotmos_xml_xpath_result result;
    rotmos_xml_name name;
    rotmos_owned_bytes bytes;
    rotmos_error error;
    int found;
    char *input = (char *)malloc(sizeof(document));
    CHECK(input != NULL);
    memcpy(input, document, sizeof(document));
    rotmos_xml_document_config_init(&config);
    CHECK(rotmos_xml_document_parse(input, sizeof(document) - 1u, &config, &doc,
                                    &error) == ROTMOS_OK);
    memset(input, 'x', sizeof(document));
    free(input);
    CHECK(rotmos_xml_document_root(doc, &root, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_name(root, &name, &error) == ROTMOS_OK &&
          strcmp(name.name, "r") == 0 &&
          strcmp(name.namespace_uri, "urn:r") == 0 && name.prefix == NULL);
    namespaces[0].prefix = "q";
    namespaces[0].uri = "urn:q";
    CHECK(rotmos_xml_document_xpath(doc, "//q:x", namespaces, 1u, &result,
                                    &error) == ROTMOS_OK &&
          result.count == 2u);
    CHECK(rotmos_xml_node_attribute(result.nodes[0], "a", NULL, &bytes, &found,
                                    &error) == ROTMOS_OK &&
          found && bytes.length == 0u);
    rotmos_owned_bytes_cleanup(&bytes);
    CHECK(rotmos_xml_node_text(result.nodes[1], &bytes, &error) == ROTMOS_OK &&
          bytes.length == 3u && memcmp(bytes.data, "a&b", 3u) == 0);
    rotmos_owned_bytes_cleanup(&bytes);
    old = result.nodes[0];
    result.nodes[0] = NULL;
    rotmos_xml_xpath_result_cleanup(&result);
    CHECK(rotmos_xml_document_xpath(doc, "count(//q:x)", namespaces, 1u,
                                    &result, &error) == ROTMOS_OK &&
          result.kind == ROTMOS_XML_XPATH_NUMBER && result.number == 2.0);
    rotmos_xml_xpath_result_cleanup(&result);
    CHECK(rotmos_xml_document_xpath(doc, "boolean(//q:x)", namespaces, 1u,
                                    &result, &error) == ROTMOS_OK &&
          result.boolean);
    rotmos_xml_xpath_result_cleanup(&result);
    CHECK(rotmos_xml_document_xpath(doc, "string(//q:x[2]/@a)", namespaces, 1u,
                                    &result, &error) == ROTMOS_OK &&
          result.string.length == 1u);
    rotmos_xml_xpath_result_cleanup(&result);
    CHECK(rotmos_xml_document_xpath(doc, "//q:x/@a", namespaces, 1u, &result,
                                    &error) == ROTMOS_INVALID);
    CHECK(rotmos_xml_document_xpath(doc, "[", NULL, 0u, &result, &error) !=
          ROTMOS_OK);
    CHECK(rotmos_xml_node_set_attribute(root, "x", "<&", 2u, "urn:q", "q",
                                        &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_name(old, &name, &error) == ROTMOS_STATE);
    CHECK(rotmos_xml_node_close(old, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_name(root, &name, &error) == ROTMOS_STATE);
    CHECK(rotmos_xml_node_close(root, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_document_root(doc, &root, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_attribute(root, "x", "urn:q", &bytes, &found,
                                    &error) == ROTMOS_OK &&
          found && bytes.length == 2u && memcmp(bytes.data, "<&", 2u) == 0);
    rotmos_owned_bytes_cleanup(&bytes);
    CHECK(rotmos_xml_node_set_attribute(root, "x", "v", 1u, "urn:other", "q",
                                        &error) == ROTMOS_CONFLICT);
    CHECK(rotmos_xml_node_set_attribute(root, "x", "v", 1u, "urn:other", "xml",
                                        &error) == ROTMOS_INVALID);
    CHECK(rotmos_xml_node_set_text(root, "\0", 1u, &error) == ROTMOS_INVALID);
    memset(&child_config, 0, sizeof(child_config));
    child_config.text = "<&";
    child_config.text_size = 2u;
    child_config.text_present = 1;
    CHECK(rotmos_xml_node_append_child(root, "item", &child_config, &child,
                                       &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_name(child, &name, &error) == ROTMOS_OK &&
          strcmp(name.namespace_uri, "urn:r") == 0);
    CHECK(rotmos_xml_node_close(root, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_set_text(child, "literal &value;",
                                   sizeof("literal &value;") - 1u,
                                   &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_close(child, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_document_serialize_materialized(doc, 1, &bytes, &error) ==
          ROTMOS_OK);
    CHECK(bytes.length > 0u);
    rotmos_owned_bytes_cleanup(&bytes);
    CHECK(rotmos_xml_document_root(doc, &root, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_remove(root, &error) == ROTMOS_INVALID);
    CHECK(rotmos_xml_document_close(doc, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_name(root, &name, &error) == ROTMOS_STATE);
    CHECK(rotmos_xml_node_close(root, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_document_parse("<r>", 3u, NULL, &doc, &error) !=
              ROTMOS_OK &&
          doc == NULL);
    CHECK(rotmos_xml_document_parse(
              "<!DOCTYPE r [<!ENTITY a 'x'>]><r>&a;</r>",
              sizeof("<!DOCTYPE r [<!ENTITY a 'x'>]><r>&a;</r>") - 1u, NULL,
              &doc, &error) != ROTMOS_OK &&
          doc == NULL);
    config.max_bytes = 2u;
    CHECK(rotmos_xml_document_parse("<r/>", 4u, &config, &doc, &error) ==
          ROTMOS_INVALID);
    rotmos_xml_document_config_init(&config);
    config.max_xpath_nodes = 1u;
    CHECK(rotmos_xml_document_parse(document, sizeof(document) - 1u, &config,
                                    &doc, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_document_xpath(doc, "//*", NULL, 0u, &result, &error) ==
          ROTMOS_INVALID);
    CHECK(rotmos_xml_document_close(doc, &error) == ROTMOS_OK);
}
typedef struct events {
    rotmos_xml_sax *parser;
    size_t starts, ends, bytes;
    int fail, reenter;
} events;
static rotmos_status start(void *context, const rotmos_xml_name *name,
                           const rotmos_xml_attribute *attrs, size_t count,
                           const rotmos_xml_namespace *ns, size_t ns_count,
                           rotmos_error *error)
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
        CHECK(rotmos_xml_sax_close(state->parser, error) == ROTMOS_STATE);
        return ROTMOS_OK;
    }
    return state->fail == 2 ? ROTMOS_AGAIN
                            : (state->fail ? ROTMOS_STATE : ROTMOS_OK);
}
static rotmos_status end(void *context, const rotmos_xml_name *name,
                         rotmos_error *error)
{
    events *state = (events *)context;
    (void)name;
    (void)error;
    ++state->ends;
    return ROTMOS_OK;
}
static rotmos_status text(void *context, const void *data, size_t size,
                          rotmos_error *error)
{
    events *state = (events *)context;
    (void)error;
    CHECK(data != NULL && size <= ROTMOS_XML_SAX_CHUNK_BYTES);
    state->bytes += size;
    return ROTMOS_OK;
}
static void native_sax(void)
{
    rotmos_xml_sax_callbacks callbacks;
    events state;
    rotmos_error error;
    char chunk[4096];
    size_t i;
    memset(&state, 0, sizeof(state));
    memset(&callbacks, 0, sizeof(callbacks));
    callbacks.start_element = start;
    callbacks.end_element = end;
    callbacks.text = text;
    CHECK(rotmos_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_xml_sax_feed(state.parser, "<r xmlns='urn:r' a='&amp;'>",
                              sizeof("<r xmlns='urn:r' a='&amp;'>") - 1u,
                              &error) == ROTMOS_OK);
    CHECK(state.starts == 1u && state.ends == 0u);
    memset(chunk, 'x', sizeof(chunk));
    for (i = 0u; i < 4097u; ++i)
        CHECK(rotmos_xml_sax_feed(state.parser, chunk, sizeof(chunk), &error) ==
              ROTMOS_OK);
    CHECK(state.bytes > ROTMOS_XML_DEFAULT_MAX_BYTES);
    CHECK(rotmos_xml_sax_feed(state.parser, "</r>", 4u, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_sax_finish(state.parser, &error) == ROTMOS_OK &&
          state.ends == 1u);
    CHECK(rotmos_xml_sax_finish(state.parser, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_sax_feed(state.parser, "", 0u, &error) == ROTMOS_STATE);
    CHECK(rotmos_xml_sax_close(state.parser, &error) == ROTMOS_OK);
    memset(&state, 0, sizeof(state));
    state.fail = 1;
    CHECK(rotmos_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_xml_sax_feed(state.parser, "<r/>", 4u, &error) ==
          ROTMOS_STATE);
    CHECK(rotmos_xml_sax_finish(state.parser, &error) == ROTMOS_STATE &&
          state.starts == 1u);
    CHECK(rotmos_xml_sax_close(state.parser, &error) == ROTMOS_OK);
    memset(&state, 0, sizeof(state));
    state.fail = 2;
    CHECK(rotmos_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_xml_sax_feed(state.parser, "<r/>", 4u, &error) ==
          ROTMOS_INVALID);
    CHECK(rotmos_xml_sax_finish(state.parser, &error) == ROTMOS_INVALID &&
          state.starts == 1u);
    CHECK(rotmos_xml_sax_close(state.parser, &error) == ROTMOS_OK);
    memset(&state, 0, sizeof(state));
    state.reenter = 1;
    CHECK(rotmos_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_xml_sax_feed(state.parser, "<r/>", 4u, &error) ==
          ROTMOS_STATE);
    CHECK(rotmos_xml_sax_close(state.parser, &error) == ROTMOS_OK);
    memset(&state, 0, sizeof(state));
    CHECK(rotmos_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_xml_sax_feed(
              state.parser, "<!DOCTYPE r SYSTEM 'file:///unused'><r/>",
              sizeof("<!DOCTYPE r SYSTEM 'file:///unused'><r/>") - 1u,
              &error) != ROTMOS_OK);
    CHECK(state.starts == 0u &&
          rotmos_xml_sax_close(state.parser, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_sax_new(&callbacks, &state, 1u, &state.parser, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_xml_sax_feed(state.parser, "<root/>", 7u, &error) ==
          ROTMOS_INVALID);
    CHECK(rotmos_xml_sax_close(state.parser, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_sax_new(&callbacks, &state, 0u, &state.parser, &error) ==
          ROTMOS_OK);
    CHECK(rotmos_xml_sax_feed(state.parser, "<r>", 3u, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_sax_finish(state.parser, &error) != ROTMOS_OK);
    CHECK(rotmos_xml_sax_close(state.parser, &error) == ROTMOS_OK);
}
static void *foreign_thread(void *context)
{
    rotmos_xml_document *doc = (rotmos_xml_document *)context;
    rotmos_xml_node *node = NULL;
    rotmos_error error;
    CHECK(rotmos_xml_document_root(doc, &node, &error) == ROTMOS_STATE &&
          node == NULL);
    CHECK(rotmos_xml_document_close(doc, &error) == ROTMOS_STATE);
    return NULL;
}
static void ownership(void)
{
    rotmos_xml_document *doc = NULL;
    rotmos_xml_node *node = NULL;
    rotmos_error error;
    pthread_t thread;
    pid_t child;
    int status;
    CHECK(rotmos_xml_document_parse("<r/>", 4u, NULL, &doc, &error) ==
          ROTMOS_OK);
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
    CHECK(rotmos_xml_document_root(doc, &node, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_node_close(node, &error) == ROTMOS_OK);
    CHECK(rotmos_xml_document_close(doc, &error) == ROTMOS_OK);
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
