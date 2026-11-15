#ifndef KNURL_PROXY_WS_REJECTION_H
#define KNURL_PROXY_WS_REJECTION_H

#include "http_framing.h"
#include "proxy_http_wire.h"

#include <knurl/proxy.h>

typedef enum knurl_proxy_ws_rejection_mode {
    KNURL_PROXY_WS_REJECTION_HEAD = 0,
    KNURL_PROXY_WS_REJECTION_FIXED = 1,
    KNURL_PROXY_WS_REJECTION_CHUNKED = 2,
    KNURL_PROXY_WS_REJECTION_CLOSE = 3,
    KNURL_PROXY_WS_REJECTION_COMPLETE = 4
} knurl_proxy_ws_rejection_mode;

typedef struct knurl_proxy_ws_rejection {
    knurl_proxy_http_response response;
    knurl_proxy_http_wire_plan wire;
    knurl_http_body_framer framer;
    knurl_proxy_ws_rejection_mode mode;
    int downstream_status;
} knurl_proxy_ws_rejection;

void knurl_proxy_ws_rejection_init(knurl_proxy_ws_rejection *rejection);
void knurl_proxy_ws_rejection_cleanup(knurl_proxy_ws_rejection *rejection);

/* Parse one already bounded HTTP/1.1 response head. An informational head
 * returns an owned wire block and leaves the parser ready for the next head.
 * A final head prepares incremental body decoding and downstream framing. */
knurl_status
knurl_proxy_ws_rejection_head(knurl_proxy_ws_rejection *rejection,
                              const unsigned char *head, size_t head_length,
                              int *final, char **wire, size_t *wire_length,
                              knurl_proxy_modify_response_fn modify,
                              void *modify_userdata, knurl_error *error);

/* Consume at most one bounded body chunk per call. The caller must drain the
 * output before calling again. COMPLETE may leave a suffix unconsumed. */
knurl_status knurl_proxy_ws_rejection_feed(knurl_proxy_ws_rejection *rejection,
                                           const unsigned char *input,
                                           size_t input_length,
                                           size_t *consumed, char *output,
                                           size_t output_capacity,
                                           size_t *written, knurl_error *error);

/* Call after framing completes, or after EOF for a close-delimited body.
 * The owned wire block contains only bounded terminal chunk and trailers. */
knurl_status
knurl_proxy_ws_rejection_finish(knurl_proxy_ws_rejection *rejection, int eof,
                                char **wire, size_t *wire_length,
                                knurl_error *error);

#endif
