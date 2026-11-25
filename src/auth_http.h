#ifndef KNURL_AUTH_HTTP_H
#define KNURL_AUTH_HTTP_H

#include "auth_internal.h"

lonejson_status knurl_auth_http(void *, const lonejson_http_request *,
                                lonejson_http_response *, lonejson_error *);
void knurl_auth_http_clear(lonejson_http_response *);
knurl_status knurl_auth_http_runtime(const char *, lonejson **, knurl_error *);

#endif
