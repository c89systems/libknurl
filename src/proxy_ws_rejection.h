#ifndef ROTMOS_PROXY_WS_REJECTION_H
#define ROTMOS_PROXY_WS_REJECTION_H

#include "proxy_framing.h"
#include "proxy_http_wire.h"

#include "proxy.h"

typedef enum rotmos_proxy_ws_rejection_mode {
    ROTMOS_PROXY_WS_REJECTION_HEAD = 0,
    ROTMOS_PROXY_WS_REJECTION_FIXED = 1,
    ROTMOS_PROXY_WS_REJECTION_CHUNKED = 2,
    ROTMOS_PROXY_WS_REJECTION_CLOSE = 3,
    ROTMOS_PROXY_WS_REJECTION_COMPLETE = 4
} rotmos_proxy_ws_rejection_mode;

typedef struct rotmos_proxy_ws_rejection {
    rotmos_proxy_http_response response;
    rotmos_proxy_http_wire_plan wire;
    rotmos_proxy_body_framer framer;
    rotmos_proxy_ws_rejection_mode mode;
    int downstream_status;
} rotmos_proxy_ws_rejection;

void rotmos_proxy_ws_rejection_init(rotmos_proxy_ws_rejection *rejection);
void rotmos_proxy_ws_rejection_cleanup(rotmos_proxy_ws_rejection *rejection);

/* Parse one already bounded HTTP/1.1 response head. An informational head
 * returns an owned wire block and leaves the parser ready for the next head.
 * A final head prepares incremental body decoding and downstream framing. */
rotmos_status
rotmos_proxy_ws_rejection_head(rotmos_proxy_ws_rejection *rejection,
                               const unsigned char *head, size_t head_length,
                               int *final, char **wire, size_t *wire_length,
                               rotmos_proxy_modify_response_fn modify,
                               void *modify_userdata, rotmos_error *error);

/* Consume at most one bounded body chunk per call. The caller must drain the
 * output before calling again. COMPLETE may leave a suffix unconsumed. */
rotmos_status rotmos_proxy_ws_rejection_feed(
    rotmos_proxy_ws_rejection *rejection, const unsigned char *input,
    size_t input_length, size_t *consumed, char *output, size_t output_capacity,
    size_t *written, rotmos_error *error);

/* Call after framing completes, or after EOF for a close-delimited body.
 * The owned wire block contains only bounded terminal chunk and trailers. */
rotmos_status
rotmos_proxy_ws_rejection_finish(rotmos_proxy_ws_rejection *rejection, int eof,
                                 char **wire, size_t *wire_length,
                                 rotmos_error *error);

#endif
