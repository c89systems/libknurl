#ifndef KNURL_PROXY_FRAMING_H
#define KNURL_PROXY_FRAMING_H

#include <stddef.h>
#include <stdint.h>

/* The caller owns each input span until feed reports it consumed. The body
 * callback may accept a prefix to apply backpressure; zero means pause. */
typedef size_t (*knurl_proxy_body_accept_fn)(void *userdata,
                                             const unsigned char *data,
                                             size_t length);

/* Trailer names and values are borrowed for the duration of the callback.
 * Return zero to reject an undeclared or otherwise disallowed field. */
typedef int (*knurl_proxy_trailer_accept_fn)(void *userdata, const char *name,
                                             const char *value);

typedef enum knurl_proxy_frame_result {
    KNURL_PROXY_FRAME_MORE = 0,
    KNURL_PROXY_FRAME_COMPLETE = 1,
    KNURL_PROXY_FRAME_PAUSED = 2,
    KNURL_PROXY_FRAME_INVALID = 3
} knurl_proxy_frame_result;

typedef enum knurl_proxy_frame_phase {
    KNURL_PROXY_PHASE_FIXED = 0,
    KNURL_PROXY_PHASE_CHUNK_SIZE = 1,
    KNURL_PROXY_PHASE_CHUNK_DATA = 2,
    KNURL_PROXY_PHASE_CHUNK_DATA_CR = 3,
    KNURL_PROXY_PHASE_CHUNK_DATA_LF = 4,
    KNURL_PROXY_PHASE_TRAILERS = 5,
    KNURL_PROXY_PHASE_COMPLETE = 6,
    KNURL_PROXY_PHASE_INVALID = 7
} knurl_proxy_frame_phase;

#define KNURL_PROXY_CHUNK_LINE_LIMIT 128u
#define KNURL_PROXY_TRAILER_LINE_LIMIT 8192u
#define KNURL_PROXY_TRAILER_BLOCK_LIMIT 16384u
#define KNURL_PROXY_TRAILER_COUNT_LIMIT 100u

typedef struct knurl_proxy_body_framer {
    knurl_proxy_frame_phase phase;
    uint64_t remaining;
    size_t line_length;
    size_t trailer_bytes;
    size_t trailer_count;
    int line_cr;
    char line[KNURL_PROXY_TRAILER_LINE_LIMIT + 1u];
} knurl_proxy_body_framer;

void knurl_proxy_body_framer_fixed(knurl_proxy_body_framer *framer,
                                   uint64_t length);
void knurl_proxy_body_framer_chunked(knurl_proxy_body_framer *framer);

/* Validate a field name for use in a trailer. The caller separately checks
 * that a request trailer was declared before forwarding it. */
int knurl_proxy_trailer_field_allowed(const char *name, size_t length);

/* Consumes only this request's body; a suffix belongs to the next request or
 * an upgraded tunnel. COMPLETE may therefore leave input unconsumed. */
knurl_proxy_frame_result knurl_proxy_body_framer_feed(
    knurl_proxy_body_framer *framer, const unsigned char *data, size_t length,
    size_t *consumed, knurl_proxy_body_accept_fn body,
    knurl_proxy_trailer_accept_fn trailer, void *userdata);

#endif
