#ifndef ROTMOS_PROXY_WS_WIRE_H
#define ROTMOS_PROXY_WS_WIRE_H

#include "proxy_headers.h"

#include <rotmos/rotmos.h>

typedef enum rotmos_proxy_ws_head_result {
    ROTMOS_PROXY_WS_HEAD_MORE = 0,
    ROTMOS_PROXY_WS_HEAD_COMPLETE = 1,
    ROTMOS_PROXY_WS_HEAD_INVALID = 2,
    ROTMOS_PROXY_WS_HEAD_LIMIT = 3,
    ROTMOS_PROXY_WS_HEAD_NOMEM = 4
} rotmos_proxy_ws_head_result;

/* Build only the bounded HTTP/1.1 opening request headers. sanitized must
 * contain end-to-end fields and no Host, framing, or Upgrade fields. */
rotmos_status
rotmos_proxy_ws_wire_request(const char *request_target, const char *authority,
                             const rotmos_proxy_headers *sanitized, char **wire,
                             size_t *wire_length, rotmos_error *error);

/* Parse a complete response head from a caller-owned bounded receive span.
 * MORE retains no bytes; the caller appends another chunk and retries.
 * COMPLETE reports the header boundary, leaving any following bytes with the
 * caller for the tunnel or rejection body. The output must be empty. */
rotmos_proxy_ws_head_result rotmos_proxy_ws_wire_response_head(
    const unsigned char *data, size_t length, size_t *head_length,
    unsigned *status, rotmos_proxy_headers *headers, const char **reason);

/* Rebuild a validated 101 response with only end-to-end upstream fields.
 * Upgrade and Connection are regenerated after sanitization. */
rotmos_status
rotmos_proxy_ws_wire_upgrade_response(const rotmos_proxy_headers *validated,
                                      char **wire, size_t *wire_length,
                                      rotmos_error *error);

#endif
