#ifndef ROTMOS_PROXY_HTTP_WIRE_H
#define ROTMOS_PROXY_HTTP_WIRE_H

#include "proxy_http.h"

#include <rotmos/rotmos.h>

typedef struct rotmos_proxy_http_wire_plan {
    char *head;
    size_t head_length;
    int chunked;
    int body_allowed;
} rotmos_proxy_http_wire_plan;

/* Format one sanitized, bounded interim response. The caller owns *wire. */
rotmos_status rotmos_proxy_http_wire_interim_build(
    int status, const rotmos_proxy_headers *headers, char **wire,
    size_t *wire_length, rotmos_error *error);

/* Format bounded HTTP/1.1 response metadata after response sanitization.
 * auto_version uses chunked output for every body-bearing response, so an
 * HTTP/2 upstream can still add trailers after declaring Content-Length. */
rotmos_status rotmos_proxy_http_wire_plan_build(
    const rotmos_proxy_http_response *response,
    const rotmos_proxy_headers *outbound_headers, int auto_version,
    rotmos_proxy_http_wire_plan *plan, rotmos_error *error);
void rotmos_proxy_http_wire_plan_cleanup(rotmos_proxy_http_wire_plan *plan);

/* Encode exactly one bounded producer chunk into caller-owned storage. */
rotmos_status
rotmos_proxy_http_wire_chunk(const rotmos_proxy_http_wire_plan *plan,
                             const unsigned char *body, size_t body_length,
                             char *buffer, size_t capacity, size_t *written,
                             rotmos_error *error);

/* Frame a held body chunk using caller-owned space before and after it.
 * The body bytes stay in place; the returned span remains valid until the
 * caller reuses or frees that storage. */
rotmos_status rotmos_proxy_http_wire_chunk_in_place(
    const rotmos_proxy_http_wire_plan *plan, unsigned char *body,
    size_t body_length, size_t headroom, size_t tailroom,
    const unsigned char **wire, size_t *written, rotmos_error *error);

/* Emit the final chunk only after the upstream transfer and trailers finish.
 * out is owned by the caller. A fixed-length response has no final bytes. */
rotmos_status
rotmos_proxy_http_wire_finish(const rotmos_proxy_http_wire_plan *plan,
                              const rotmos_proxy_headers *trailers, char **out,
                              size_t *out_length, rotmos_error *error);

#endif
