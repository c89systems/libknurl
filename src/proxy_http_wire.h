#ifndef KNURL_PROXY_HTTP_WIRE_H
#define KNURL_PROXY_HTTP_WIRE_H

#include "proxy_http.h"

#include <knurl/knurl.h>

typedef struct knurl_proxy_http_wire_plan {
    char *head;
    size_t head_length;
    int chunked;
    int body_allowed;
} knurl_proxy_http_wire_plan;

/* Format one sanitized, bounded interim response. The caller owns *wire. */
knurl_status knurl_proxy_http_wire_interim_build(
    int status, const knurl_proxy_headers *headers, char **wire,
    size_t *wire_length, knurl_error *error);

/* Format bounded HTTP/1.1 response metadata after response sanitization.
 * auto_version uses chunked output for every body-bearing response, so an
 * HTTP/2 upstream can still add trailers after declaring Content-Length. */
knurl_status knurl_proxy_http_wire_plan_build(
    const knurl_proxy_http_response *response,
    const knurl_proxy_headers *outbound_headers, int auto_version,
    knurl_proxy_http_wire_plan *plan, knurl_error *error);
void knurl_proxy_http_wire_plan_cleanup(knurl_proxy_http_wire_plan *plan);

/* Encode exactly one bounded producer chunk into caller-owned storage. */
knurl_status knurl_proxy_http_wire_chunk(const knurl_proxy_http_wire_plan *plan,
                                         const unsigned char *body,
                                         size_t body_length, char *buffer,
                                         size_t capacity, size_t *written,
                                         knurl_error *error);

/* Frame a held body chunk using caller-owned space before and after it.
 * The body bytes stay in place; the returned span remains valid until the
 * caller reuses or frees that storage. */
knurl_status knurl_proxy_http_wire_chunk_in_place(
    const knurl_proxy_http_wire_plan *plan, unsigned char *body,
    size_t body_length, size_t headroom, size_t tailroom,
    const unsigned char **wire, size_t *written, knurl_error *error);

/* Emit the final chunk only after the upstream transfer and trailers finish.
 * out is owned by the caller. A fixed-length response has no final bytes. */
knurl_status
knurl_proxy_http_wire_finish(const knurl_proxy_http_wire_plan *plan,
                             const knurl_proxy_headers *trailers, char **out,
                             size_t *out_length, knurl_error *error);

#endif
