#include "static_internal.h"
#include <zlib.h>
#include <stdlib.h>
#include <string.h>

/* Bounded transport-sized chunks, never a whole representation. */
struct gzip_source {
    knurl_source *input;
    z_stream stream;
    gz_header header;
    unsigned char input_bytes[8192], output_bytes[16384];
    size_t output_at, output_size;
    int eof, done;
};
static knurl_status gzip_error(knurl_error *error, int code)
{
    knurl_status status = code == Z_MEM_ERROR ? KNURL_NOMEM : KNURL_IO;
    (void)knurl_fail(error, status, "static_gzip", "gzip stream failed");
    if (error != NULL)
        error->native_code = code;
    return status;
}
static knurl_status reset_gzip(void *data, knurl_error *error)
{
    struct gzip_source *s = data;
    knurl_status status;
    int code;
    status = knurl_source_reset(s->input, error);
    if (status != KNURL_OK)
        return status;
    code = deflateReset(&s->stream);
    if (code != Z_OK)
        return gzip_error(error, code);
    s->stream.next_in = Z_NULL;
    s->stream.avail_in = 0;
    s->eof = s->done = 0;
    s->output_at = s->output_size = 0;
    memset(&s->header, 0, sizeof(s->header));
    s->header.os = 255;
    code = deflateSetHeader(&s->stream, &s->header);
    return code == Z_OK ? KNURL_OK : gzip_error(error, code);
}
static knurl_status read_gzip(void *bytes, size_t capacity, size_t *produced,
                              void *data, knurl_error *error)
{
    struct gzip_source *s = data;
    size_t count;
    knurl_status status;
    int code;
    *produced = 0;
    if (s->output_at == s->output_size) {
        if (s->done)
            return KNURL_EOF;
        if (!s->eof && s->stream.avail_in == 0) {
            status = knurl_source_read(s->input, s->input_bytes,
                                       sizeof(s->input_bytes), &count, error);
            if (status == KNURL_EOF)
                s->eof = 1;
            else if (status != KNURL_OK)
                return status;
            else {
                s->stream.next_in = s->input_bytes;
                s->stream.avail_in = (uInt)count;
            }
        }
        s->stream.next_out = s->output_bytes;
        s->stream.avail_out = sizeof(s->output_bytes);
        code = deflate(&s->stream, s->eof ? Z_FINISH : Z_SYNC_FLUSH);
        if (code != Z_OK && code != Z_STREAM_END)
            return gzip_error(error, code);
        s->done = code == Z_STREAM_END;
        s->output_at = 0;
        s->output_size = sizeof(s->output_bytes) - s->stream.avail_out;
        if (s->output_size == 0)
            return s->done ? KNURL_EOF
                           : knurl_fail(error, KNURL_IO, "static_gzip",
                                        "gzip made no bounded-chunk progress");
    }
    count = s->output_size - s->output_at;
    if (count > capacity)
        count = capacity;
    memcpy(bytes, s->output_bytes + s->output_at, count);
    s->output_at += count;
    *produced = count;
    return KNURL_OK;
}
static void close_gzip(void *data, knurl_status outcome)
{
    struct gzip_source *s = data;
    (void)outcome;
    (void)deflateEnd(&s->stream);
    (void)knurl_source_destroy(s->input, NULL);
    free(s);
}
knurl_status knurl_static_gzip_take(knurl_source *input, knurl_source **out,
                                    knurl_error *error)
{
    struct gzip_source *s;
    knurl_source_config config;
    knurl_status status;
    int code;
    if (input == NULL || out == NULL)
        return knurl_fail(error, KNURL_INVALID, "static_gzip",
                          "provide input and output");
    *out = NULL;
    s = calloc(1, sizeof(*s));
    if (s == NULL)
        return knurl_fail(error, KNURL_NOMEM, "static_gzip",
                          "cannot allocate gzip chunks");
    s->input = input;
    code = deflateInit2(&s->stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED,
                        MAX_WBITS + 16, 8, Z_DEFAULT_STRATEGY);
    if (code != Z_OK) {
        free(s);
        return gzip_error(error, code);
    }
    s->header.os = 255;
    code = deflateSetHeader(&s->stream, &s->header);
    if (code != Z_OK) {
        (void)deflateEnd(&s->stream);
        free(s);
        return gzip_error(error, code);
    }
    knurl_source_config_init(&config);
    config.read = read_gzip;
    config.reset = reset_gzip;
    config.close = close_gzip;
    config.reset_before_reply = 1;
    config.userdata = s;
    status = knurl_source_new(&config, out, error);
    if (status != KNURL_OK) {
        (void)deflateEnd(&s->stream);
        free(s);
    }
    return status;
}
