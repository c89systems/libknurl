#define _POSIX_C_SOURCE 200809L
#include "static_internal.h"
#include <knurl/input.h>
#include <curl/curl.h>
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Compile the same production implementations with failing allocator/source
 * boundaries. Public operations and ownership outcomes remain the assertions.
 */
static int fail_malloc, fail_calloc, fail_source;
static void *fault_malloc(size_t n);
static void *fault_calloc(size_t n, size_t size);
static knurl_status fault_source(const knurl_source_config *, knurl_source **,
                                 knurl_error *);
#define malloc fault_malloc
#define calloc fault_calloc
#define knurl_source_new fault_source
#include "../src/input.c"
#include "../src/static_gzip.c"
#include "../src/static_multipart.c"
#undef malloc
#undef calloc
#undef knurl_source_new
static void *fault_malloc(size_t n)
{
    if (fail_malloc) {
        fail_malloc = 0;
        return NULL;
    }
    return malloc(n);
}
static void *fault_calloc(size_t n, size_t size)
{
    if (fail_calloc) {
        fail_calloc = 0;
        return NULL;
    }
    return calloc(n, size);
}
static knurl_status fault_source(const knurl_source_config *config,
                                 knurl_source **out, knurl_error *error)
{
    if (fail_source) {
        fail_source = 0;
        *out = NULL;
        return knurl_fail(error, KNURL_NOMEM, "fixture",
                          "source allocation failed");
    }
    return knurl_source_new(config, out, error);
}
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "HTTP allocation %d: %s\n", __LINE__, #t);         \
            return 1;                                                          \
        }                                                                      \
    } while (0)
static void closed(void *data) { ++*(int *)data; }
int main(void)
{
    knurl_owned_bytes escaped;
    knurl_bytes input;
    knurl_source *source, *compressed;
    knurl_asset_info info;
    knurl_asset *asset;
    struct knurl_static_ranges ranges;
    knurl_error error;
    int count = 0, turn;
    input.data = "<tag>";
    input.length = 5;
    knurl_owned_bytes_init(&escaped);
    fail_malloc = 1;
    CHECK(knurl_input_html_escape(input, 0, &escaped, &error) == KNURL_NOMEM &&
          escaped.data == NULL && escaped.length == 0);
    input.data = "https://example.org";
    input.length = strlen(input.data);
    fail_malloc = 1;
    CHECK(knurl_input_http_url(input, &error) == KNURL_NOMEM);
    input.data = "abcdef";
    input.length = 6;
    CHECK(knurl_source_bytes(input, &source, &error) == KNURL_OK);
    for (turn = 0; turn < 2; ++turn) {
        fail_calloc = turn == 0;
        fail_source = turn == 1;
        CHECK(knurl_static_gzip_take(source, &compressed, &error) ==
                  KNURL_NOMEM &&
              compressed == NULL);
        CHECK(source->reset(source, &error) == KNURL_OK);
    }
    CHECK(source->destroy(source, &error) == KNURL_OK);
    memset(&info, 0, sizeof(info));
    info.size.low = 6;
    CHECK(knurl_asset_memory(&info, input, closed, &count, &asset, &error) ==
          KNURL_OK);
    knurl_static_range_parse("bytes=0-1,4-5", info.size, &ranges);
    for (turn = 0; turn < 2; ++turn) {
        fail_calloc = turn == 0;
        fail_source = turn == 1;
        CHECK(knurl_static_multipart_take(asset, &ranges, "text/plain", "test",
                                          &source, &error) == KNURL_NOMEM &&
              source == NULL);
        CHECK(count == 0);
    }
    CHECK(asset->close(asset, &error) == KNURL_OK && count == 1);
    return 0;
}
