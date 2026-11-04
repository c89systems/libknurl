#ifndef ROTMOS_PROXY_FRAMING_H
#define ROTMOS_PROXY_FRAMING_H

#include <stddef.h>
#include <stdint.h>

/* The caller owns each input span until feed reports it consumed. The body
 * callback may accept a prefix to apply backpressure; zero means pause. */
typedef size_t (*rotmos_proxy_body_accept_fn)(void *userdata,
                                              const unsigned char *data,
                                              size_t length);

/* Trailer names and values are borrowed for the duration of the callback.
 * Return zero to reject an undeclared or otherwise disallowed field. */
typedef int (*rotmos_proxy_trailer_accept_fn)(void *userdata, const char *name,
                                              const char *value);

typedef enum rotmos_proxy_frame_result {
    ROTMOS_PROXY_FRAME_MORE = 0,
    ROTMOS_PROXY_FRAME_COMPLETE = 1,
    ROTMOS_PROXY_FRAME_PAUSED = 2,
    ROTMOS_PROXY_FRAME_INVALID = 3
} rotmos_proxy_frame_result;

typedef enum rotmos_proxy_frame_phase {
    ROTMOS_PROXY_PHASE_FIXED = 0,
    ROTMOS_PROXY_PHASE_CHUNK_SIZE = 1,
    ROTMOS_PROXY_PHASE_CHUNK_DATA = 2,
    ROTMOS_PROXY_PHASE_CHUNK_DATA_CR = 3,
    ROTMOS_PROXY_PHASE_CHUNK_DATA_LF = 4,
    ROTMOS_PROXY_PHASE_TRAILERS = 5,
    ROTMOS_PROXY_PHASE_COMPLETE = 6,
    ROTMOS_PROXY_PHASE_INVALID = 7
} rotmos_proxy_frame_phase;

#define ROTMOS_PROXY_CHUNK_LINE_LIMIT 128u
#define ROTMOS_PROXY_TRAILER_LINE_LIMIT 8192u
#define ROTMOS_PROXY_TRAILER_BLOCK_LIMIT 16384u
#define ROTMOS_PROXY_TRAILER_COUNT_LIMIT 100u

typedef struct rotmos_proxy_body_framer {
    rotmos_proxy_frame_phase phase;
    uint64_t remaining;
    size_t line_length;
    size_t trailer_bytes;
    size_t trailer_count;
    int line_cr;
    char line[ROTMOS_PROXY_TRAILER_LINE_LIMIT + 1u];
} rotmos_proxy_body_framer;

void rotmos_proxy_body_framer_fixed(rotmos_proxy_body_framer *framer,
                                    uint64_t length);
void rotmos_proxy_body_framer_chunked(rotmos_proxy_body_framer *framer);

/* Validate a field name for use in a trailer. The caller separately checks
 * that a request trailer was declared before forwarding it. */
int rotmos_proxy_trailer_field_allowed(const char *name, size_t length);

/* Consumes only this request's body; a suffix belongs to the next request or
 * an upgraded tunnel. COMPLETE may therefore leave input unconsumed. */
rotmos_proxy_frame_result rotmos_proxy_body_framer_feed(
    rotmos_proxy_body_framer *framer, const unsigned char *data, size_t length,
    size_t *consumed, rotmos_proxy_body_accept_fn body,
    rotmos_proxy_trailer_accept_fn trailer, void *userdata);

#endif
