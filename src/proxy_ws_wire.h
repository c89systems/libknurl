#ifndef KNURL_PROXY_WS_WIRE_H
#define KNURL_PROXY_WS_WIRE_H

#include "proxy_headers.h"

#include <knurl/knurl.h>

typedef enum knurl_proxy_ws_head_result {
    KNURL_PROXY_WS_HEAD_MORE = 0,
    KNURL_PROXY_WS_HEAD_COMPLETE = 1,
    KNURL_PROXY_WS_HEAD_INVALID = 2,
    KNURL_PROXY_WS_HEAD_LIMIT = 3,
    KNURL_PROXY_WS_HEAD_NOMEM = 4
} knurl_proxy_ws_head_result;

/* Build only the bounded HTTP/1.1 opening request headers. sanitized must
 * contain end-to-end fields and no Host, framing, or Upgrade fields. */
knurl_status knurl_proxy_ws_wire_request(const char *request_target,
                                         const char *authority,
                                         const knurl_proxy_headers *sanitized,
                                         char **wire, size_t *wire_length,
                                         knurl_error *error);

/* Parse a complete response head from a caller-owned bounded receive span.
 * MORE retains no bytes; the caller appends another chunk and retries.
 * COMPLETE reports the header boundary, leaving any following bytes with the
 * caller for the tunnel or rejection body. The output must be empty. */
knurl_proxy_ws_head_result knurl_proxy_ws_wire_response_head(
    const unsigned char *data, size_t length, size_t *head_length,
    unsigned *status, knurl_proxy_headers *headers, const char **reason);

/* Rebuild a validated 101 response with only end-to-end upstream fields.
 * Upgrade and Connection are regenerated after sanitization. */
knurl_status
knurl_proxy_ws_wire_upgrade_response(const knurl_proxy_headers *validated,
                                     char **wire, size_t *wire_length,
                                     knurl_error *error);

#endif
