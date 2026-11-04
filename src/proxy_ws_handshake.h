#ifndef ROTMOS_PROXY_WS_HANDSHAKE_H
#define ROTMOS_PROXY_WS_HANDSHAKE_H

#include "proxy_headers.h"

#include <rotmos/rotmos.h>

/* Validate the HTTP/1.1 opening request before a connect-only upstream is
 * submitted. Header views are borrowed for the duration of the call. */
int rotmos_proxy_ws_request_valid(rotmos_http_method method,
                                  const rotmos_proxy_headers *headers,
                                  const char **reason);

/* Validate an upstream 101 against the original client offer. Non-101
 * responses take the ordinary HTTP rejection path instead. */
int rotmos_proxy_ws_response_valid(const rotmos_proxy_headers *request,
                                   unsigned status,
                                   const rotmos_proxy_headers *response,
                                   const char **reason);

#endif
