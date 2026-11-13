#include "static_internal.h"
#include "request_internal.h"
#include "receivers_internal.h"
#include <zlib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "encoding %d: %s\n", __LINE__, #t);                \
            return 1;                                                          \
        }                                                                      \
    } while (0)

struct producer {
    size_t offset, maximum;
    int again, closed;
};
static unsigned char original[65536], encoded[131072], repeated[131072],
    decoded[65536];
static knurl_status produce(void *buffer, size_t capacity, size_t *count,
                            void *data, knurl_error *error)
{
    struct producer *p = data;
    (void)error;
    *count = 0;
    if (capacity > p->maximum)
        p->maximum = capacity;
    if (p->again) {
        p->again = 0;
        return KNURL_AGAIN;
    }
    if (p->offset == sizeof(original))
        return KNURL_EOF;
    *count = sizeof(original) - p->offset;
    if (*count > capacity)
        *count = capacity;
    memcpy(buffer, original + p->offset, *count);
    p->offset += *count;
    return KNURL_OK;
}
static knurl_status rewind_input(void *data, knurl_error *error)
{
    (void)error;
    ((struct producer *)data)->offset = 0;
    return KNURL_OK;
}
static void close_input(void *data, knurl_status outcome)
{
    (void)outcome;
    ++((struct producer *)data)->closed;
}
static int gzip_stream(void)
{
    struct producer p;
    knurl_source_config config;
    knurl_source *input, *gzip;
    knurl_status status;
    knurl_error error;
    z_stream decoder;
    size_t i, turn, count, total = 0, saved = 0;
    unsigned char *destination;
    memset(&p, 0, sizeof(p));
    for (i = 0; i < sizeof(original); ++i)
        original[i] = (unsigned char)((i * 7919UL + i / 7) & 255);
    knurl_source_config_init(&config);
    config.read = produce;
    config.reset = rewind_input;
    config.close = close_input;
    config.userdata = &p;
    CHECK(knurl_source_new(&config, &input, &error) == KNURL_OK);
    CHECK(knurl_static_gzip_take(input, &gzip, &error) == KNURL_OK);
    CHECK(p.offset == 0 && p.closed == 0);
    for (turn = 0; turn < 2; ++turn) {
        destination = turn == 0 ? encoded : repeated;
        total = 0;
        p.again = 1;
        CHECK(gzip->read(gzip, destination, 3, &count, &error) == KNURL_AGAIN &&
              count == 0);
        CHECK(gzip->read(gzip, destination, 3, &count, &error) == KNURL_OK &&
              count > 0);
        CHECK(p.offset < sizeof(original) && p.maximum <= 8192);
        total += count;
        for (;;) {
            CHECK(total + 3 <= sizeof(encoded));
            status = gzip->read(gzip, destination + total, 3, &count, &error);
            if (status == KNURL_EOF)
                break;
            CHECK(status == KNURL_OK && count > 0 && count <= 3);
            total += count;
        }
        if (turn == 0)
            saved = total;
        else
            CHECK(saved == total && memcmp(encoded, repeated, total) == 0);
        CHECK(gzip->reset(gzip, &error) == KNURL_OK && p.offset == 0);
    }
    memset(&decoder, 0, sizeof(decoder));
    CHECK(inflateInit2(&decoder, MAX_WBITS + 16) == Z_OK);
    decoder.next_in = encoded;
    decoder.avail_in = (uInt)total;
    decoder.next_out = decoded;
    decoder.avail_out = sizeof(decoded);
    CHECK(inflate(&decoder, Z_FINISH) == Z_STREAM_END);
    CHECK(decoder.total_out == sizeof(original) && decoder.avail_in == 0 &&
          memcmp(original, decoded, sizeof(original)) == 0);
    CHECK(inflateEnd(&decoder) == Z_OK);
    CHECK(gzip->destroy(gzip, &error) == KNURL_OK && p.closed == 1);
    return 0;
}
struct provider_state {
    int gzip, br, stale, opened, closed;
};
static void close_asset(void *data)
{
    ++((struct provider_state *)data)->closed;
}
static knurl_status open_asset(const char *path, knurl_asset **out, void *data,
                               knurl_error *error)
{
    struct provider_state *p = data;
    knurl_asset_info info;
    knurl_bytes bytes;
    if ((strcmp(path, "/a.gz") != 0 || !p->gzip) &&
        (strcmp(path, "/a.br") != 0 || !p->br) && strcmp(path, "/a") != 0)
        return KNURL_EOF;
    memset(&info, 0, sizeof(info));
    info.etag = "\"version\"";
    info.size.low = 6;
    info.has_modification_time = 1;
    info.modification_seconds.low =
        strcmp(path, "/a") == 0 || !p->stale ? 10 : 9;
    bytes.data = "abcdef";
    bytes.length = 6;
    ++p->opened;
    return knurl_asset_memory(&info, bytes, close_asset, p, out, error);
}
static knurl_bytes span(const char *s)
{
    knurl_bytes b;
    b.data = s;
    b.length = strlen(s);
    return b;
}
static int negotiation(const char *accept, const char *extra_name,
                       const char *extra_value, int gzip, int br, int stale,
                       int disabled, int pre_disabled, const char *type,
                       knurl_status expected, const char *coding, int dynamic)
{
    struct provider_state p;
    struct knurl_request_impl request;
    struct knurl_static_encoding chosen;
    knurl_static_config config;
    knurl_asset_provider_config provider_config;
    knurl_asset_provider *provider;
    knurl_asset *identity;
    knurl_reply *reply;
    knurl_header headers[2], h;
    knurl_error error;
    size_t i;
    int vary_count = 0;
    memset(&p, 0, sizeof(p));
    p.gzip = gzip;
    p.br = br;
    p.stale = stale;
    knurl_asset_provider_config_init(&provider_config);
    provider_config.open = open_asset;
    provider_config.userdata = &p;
    CHECK(knurl_asset_provider_new(&provider_config, &provider, &error) ==
          KNURL_OK);
    CHECK(provider->open(provider, "/a", &identity, &error) == KNURL_OK);
    memset(&request, 0, sizeof(request));
    knurl_request_receiver_init(&request.receiver);
    request.method = KNURL_HTTP_GET;
    request.headers = headers;
    if (accept != NULL) {
        headers[request.header_count].name = span("Accept-Encoding");
        headers[request.header_count++].value = span(accept);
    }
    if (extra_name != NULL) {
        headers[request.header_count].name = span(extra_name);
        headers[request.header_count++].value = span(extra_value);
    }
    knurl_static_config_init(&config);
    config.compression_disabled = disabled;
    config.precompressed_disabled = pre_disabled;
    CHECK(knurl_reply_new(&reply, &error) == KNURL_OK);
    CHECK(reply->header_add(reply, "vary", "Origin", &error) == KNURL_OK);
    CHECK(knurl_static_encoding_select(&config, provider, &request.receiver,
                                       reply, identity, "/a", type, &chosen,
                                       &error) == expected);
    if (expected == KNURL_OK) {
        CHECK((coding == NULL && chosen.coding == NULL) ||
              (coding != NULL && chosen.coding != NULL &&
               strcmp(coding, chosen.coding) == 0));
        CHECK(chosen.gzip == dynamic);
        if (coding != NULL)
            CHECK(strstr(chosen.info.etag, coding) != NULL &&
                  (dynamic ? strncmp(chosen.info.etag, "W/", 2) == 0
                           : chosen.info.etag[0] == '"'));
        for (i = 0; i < reply->header_count(reply); ++i) {
            CHECK(reply->header_at(reply, i, &h, &error) == KNURL_OK);
            if (strcmp(h.name.data, "vary") == 0)
                ++vary_count;
        }
        CHECK(vary_count == (disabled ? 1 : 2));
    }
    if (chosen.asset != identity)
        CHECK(chosen.asset->close(chosen.asset, &error) == KNURL_OK);
    CHECK(identity->close(identity, &error) == KNURL_OK);
    CHECK(reply->destroy(reply, &error) == KNURL_OK);
    CHECK(provider->close(provider, &error) == KNURL_OK);
    CHECK(p.opened == p.closed);
    return 0;
}
int main(void)
{
    CHECK(gzip_stream() == 0);
#define CASE(a, n, v, g, b, s, d, p, t, e, c, z)                               \
    CHECK(negotiation(a, n, v, g, b, s, d, p, t, e, c, z) == 0)
    CASE(NULL, NULL, NULL, 1, 1, 0, 0, 0, "text/plain", KNURL_OK, NULL, 0);
    CASE("gzip,br", NULL, NULL, 1, 1, 0, 0, 0, "text/plain", KNURL_OK, "br", 0);
    CASE("gzip,br;q=0.5", NULL, NULL, 1, 1, 0, 0, 0, "text/plain", KNURL_OK,
         "gzip", 0);
    CASE("gzip", NULL, NULL, 0, 0, 0, 0, 0, "text/plain", KNURL_OK, "gzip", 1);
    CASE("gzip", NULL, NULL, 1, 1, 1, 0, 0, "text/plain", KNURL_OK, "gzip", 1);
    CASE("gzip,br", NULL, NULL, 1, 1, 0, 0, 1, "text/plain", KNURL_OK, "gzip",
         1);
    CASE("gzip", NULL, NULL, 1, 1, 0, 1, 0, "text/plain", KNURL_OK, NULL, 0);
    CASE("gzip", "range", "bytes=0-1", 1, 1, 0, 0, 0, "text/plain", KNURL_OK,
         NULL, 0);
    CASE("gzip,identity;q=0", "range", "bytes=0-1", 1, 1, 0, 0, 0, "text/plain",
         KNURL_EOF, NULL, 0);
    CASE("gzip", "cache-control", "private, no-transform", 1, 1, 0, 0, 0,
         "text/plain", KNURL_OK, NULL, 0);
    CASE("*;q=0", NULL, NULL, 1, 1, 0, 0, 0, "text/plain", KNURL_EOF, NULL, 0);
    CASE("gzip;q=0.5,identity;q=0.1", NULL, NULL, 0, 0, 0, 0, 0,
         "application/problem+json", KNURL_OK, "gzip", 1);
    CASE("gzip;q=0.5", NULL, NULL, 1, 1, 0, 0, 0, "text/plain", KNURL_OK, NULL,
         0);
    CASE("gzip,identity;q=0", NULL, NULL, 0, 0, 0, 0, 0, "image/png", KNURL_EOF,
         NULL, 0);
    CASE("gzip;q=1.1", NULL, NULL, 0, 0, 0, 0, 0, "text/plain", KNURL_INVALID,
         NULL, 0);
    CASE("gzip;q=0.1234", NULL, NULL, 0, 0, 0, 0, 0, "text/plain",
         KNURL_INVALID, NULL, 0);
    CASE("gzip,gzip", NULL, NULL, 0, 0, 0, 0, 0, "text/plain", KNURL_INVALID,
         NULL, 0);
    CASE("gzip,identity;q=0", NULL, NULL, 0, 0, 0, 0, 0,
         " text/plain ;charset=utf-8", KNURL_OK, "gzip", 1);
    return 0;
}
