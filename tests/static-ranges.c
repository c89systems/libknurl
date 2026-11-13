#include "static_internal.h"
#include "source_internal.h"
#include <stdio.h>
#include <string.h>
#define CHECK(t)                                                               \
    do {                                                                       \
        if (!(t)) {                                                            \
            fprintf(stderr, "ranges %d: %s\n", __LINE__, #t);                  \
            return 1;                                                          \
        }                                                                      \
    } while (0)
struct fixture {
    int reads, closed, wide, again;
};
static knurl_status read_bytes(knurl_u64 offset, void *buffer, size_t capacity,
                               size_t *produced, void *data, knurl_error *e)
{
    struct fixture *f = data;
    size_t n;
    (void)e;
    ++f->reads;
    *produced = 0;
    if (f->again) {
        f->again = 0;
        return KNURL_AGAIN;
    }
    if (offset.high != (unsigned long)f->wide || offset.low >= 6)
        return KNURL_IO;
    n = 6 - (size_t)offset.low;
    if (n > capacity)
        n = capacity;
    memcpy(buffer, &"abcdef"[offset.low], n);
    *produced = n;
    return KNURL_OK;
}
static void close_asset(void *data) { ((struct fixture *)data)->closed++; }
static int parser(void)
{
    struct knurl_static_ranges ranges;
    knurl_u64 length = {0, 6};
    static const char *const invalid[] = {
        "bytes=",
        "bytes=1-0",
        "bytes=0-1,",
        "bytes=0-1,,3-4",
        "bytes=bad",
        "bytes=-0",
        "bytes=6-",
        "bytes=0-18446744073709551616",
        ("bytes=0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-0,0-"
         "0,"
         "0-0")};
    size_t i;
    knurl_static_range_parse("bytes=0-1,99-,-2", length, &ranges);
    CHECK(ranges.requested && ranges.satisfiable && ranges.count == 2);
    CHECK(ranges.parts[0].start.low == 0 && ranges.parts[0].length.low == 2);
    CHECK(ranges.parts[1].start.low == 4 && ranges.parts[1].length.low == 2);
    for (i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
        knurl_static_range_parse(invalid[i], length, &ranges);
        CHECK(ranges.requested && !ranges.satisfiable && ranges.count == 0);
    }
    knurl_static_range_parse("items=0-1", length, &ranges);
    CHECK(!ranges.requested);
    knurl_static_range_parse("ByTeS=0-1", length, &ranges);
    CHECK(ranges.requested && ranges.satisfiable && ranges.count == 1);
    length.low = 0;
    knurl_static_range_parse("bytes=0-", length, &ranges);
    CHECK(ranges.requested && !ranges.satisfiable);
    return 0;
}
static int source(int wide)
{
    struct fixture f;
    knurl_asset_config config;
    knurl_asset *asset;
    knurl_source *stream;
    knurl_error error;
    struct knurl_static_ranges ranges;
    char expected[512], actual[512], chunk[7];
    size_t count, total, turn;
    knurl_status status;
    memset(&f, 0, sizeof(f));
    f.wide = wide;
    knurl_asset_config_init(&config);
    config.info.size.high = (unsigned long)wide;
    config.info.size.low = 6;
    config.read = read_bytes;
    config.close = close_asset;
    config.userdata = &f;
    CHECK(knurl_asset_new(&config, &asset, &error) == KNURL_OK);
    knurl_static_range_parse(wide ? "bytes=4294967296-4294967297,4294967300-"
                                  : "bytes=0-1,4-",
                             config.info.size, &ranges);
    CHECK(knurl_static_multipart_take(asset, &ranges, "text/plain", "unit",
                                      &stream, &error) == KNURL_OK);
    CHECK(f.reads == 0 && f.closed == 0);
    if (wide)
        strcpy(expected,
               "--unit\r\nContent-Type: text/plain\r\nContent-Range: bytes "
               "4294967296-4294967297/"
               "4294967302\r\n\r\nab\r\n--unit\r\nContent-Type: "
               "text/plain\r\nContent-Range: bytes "
               "4294967300-4294967301/4294967302\r\n\r\nef\r\n--unit--\r\n");
    else
        strcpy(expected, "--unit\r\nContent-Type: text/plain\r\nContent-Range: "
                         "bytes 0-1/6\r\n\r\nab\r\n--unit\r\nContent-Type: "
                         "text/plain\r\nContent-Range: bytes "
                         "4-5/6\r\n\r\nef\r\n--unit--\r\n");
    CHECK(asset->close(asset, &error) == KNURL_STATE && f.closed == 0);
    for (turn = 0; turn < 2; ++turn) {
        total = 0;
        f.again = 1;
        CHECK(stream->read(stream, chunk, sizeof(chunk), &count, &error) ==
                  KNURL_OK &&
              count == 7);
        CHECK(turn != 0 || f.reads == 0);
        memcpy(actual, chunk, count);
        total += count;
        for (;;) {
            status = stream->read(stream, chunk, sizeof(chunk), &count, &error);
            if (status == KNURL_EOF)
                break;
            if (status == KNURL_AGAIN) {
                CHECK(count == 0);
                continue;
            }
            CHECK(status == KNURL_OK && count > 0 &&
                  total + count < sizeof(actual));
            memcpy(actual + total, chunk, count);
            total += count;
        }
        CHECK(total == strlen(expected) &&
              memcmp(actual, expected, total) == 0);
        CHECK(stream->reset(stream, &error) == KNURL_OK);
    }
    CHECK(stream->destroy(stream, &error) == KNURL_OK && f.closed == 1);
    return 0;
}
static int wide_total(void)
{
    struct fixture f;
    struct knurl_static_ranges ranges;
    knurl_asset_config config;
    knurl_asset *asset;
    knurl_source *source;
    knurl_error error;
    char head[512];
    size_t n;
    memset(&f, 0, sizeof(f));
    knurl_asset_config_init(&config);
    config.info.size.high = 1;
    config.info.size.low = 6;
    config.read = read_bytes;
    config.close = close_asset;
    config.userdata = &f;
    CHECK(knurl_asset_new(&config, &asset, &error) == KNURL_OK);
    knurl_static_range_parse("bytes=0-4294967295,4294967296-", config.info.size,
                             &ranges);
    CHECK(knurl_static_multipart_take(asset, &ranges, "text/plain", "wide",
                                      &source, &error) == KNURL_OK);
    /* Declared length is externally used as Content-Length. Verify exact wide
     * arithmetic without generating a 4 GiB fixture on a 32-bit runner. */
    CHECK(source->impl->config.has_length &&
          source->impl->config.length.high == 1 &&
          source->impl->config.length.low == 193);
    CHECK(source->read(source, head, 1, &n, &error) == KNURL_OK && n == 1 &&
          f.reads == 0);
    CHECK(source->destroy(source, &error) == KNURL_OK && f.closed == 1);
    return 0;
}
static int dates(void)
{
    knurl_asset_info info;
    memset(&info, 0, sizeof(info));
    info.etag = "\"tag\"";
    info.last_modified = "Sun, 06 Nov 1994 08:49:37 GMT";
    CHECK(knurl_static_if_range("\"tag\"", &info));
    CHECK(!knurl_static_if_range("W/\"tag\"", &info));
    CHECK(!knurl_static_if_range(info.last_modified, &info));
    info.last_modified_strong = 1;
    CHECK(knurl_static_if_range(info.last_modified, &info));
    CHECK(knurl_static_if_range("Sunday, 06-Nov-94 08:49:37 GMT", &info));
    CHECK(knurl_static_if_range("Sun Nov  6 08:49:37 1994", &info));
    CHECK(knurl_static_if_range(" \tSun, 06 Nov 1994 08:49:37 GMT \t", &info));
    CHECK(!knurl_static_if_range("06 Nov 1994 08:49:37 GMT", &info));
    CHECK(!knurl_static_if_range("Sun, 06 Nov 1994 08:49:37 GMT, "
                                 "Sun, 06 Nov 1994 08:49:37 GMT",
                                 &info));
    CHECK(!knurl_static_if_range("Sun, 06 Nov 1994 08:49:36 GMT", &info));
    CHECK(!knurl_static_if_range("bad", &info));
    info.etag = "W/\"tag\"";
    CHECK(!knurl_static_if_range("\"tag\"", &info));
    return 0;
}
int main(void)
{
    CHECK(parser() == 0);
    CHECK(source(0) == 0);
    CHECK(source(1) == 0);
    CHECK(dates() == 0);
    CHECK(wide_total() == 0);
    return 0;
}
