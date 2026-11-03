#ifndef ROTMOS_REQUEST_H
#define ROTMOS_REQUEST_H

#include <rotmos/http.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct rotmos_endpoint {
    rotmos_bytes address;
    unsigned short port;
} rotmos_endpoint;

ROTMOS_API rotmos_server *rotmos_request_server(const rotmos_request *request);
ROTMOS_API rotmos_http_method
rotmos_request_method(const rotmos_request *request);
ROTMOS_API rotmos_bytes
rotmos_request_method_token(const rotmos_request *request);
ROTMOS_API rotmos_bytes rotmos_request_version(const rotmos_request *request);
ROTMOS_API rotmos_bytes rotmos_request_target(const rotmos_request *request);
ROTMOS_API rotmos_bytes rotmos_request_raw_path(const rotmos_request *request);
ROTMOS_API rotmos_bytes rotmos_request_path(const rotmos_request *request);
ROTMOS_API rotmos_bytes rotmos_request_query(const rotmos_request *request);
ROTMOS_API rotmos_bytes rotmos_request_authority(const rotmos_request *request);
ROTMOS_API size_t rotmos_request_listener_index(const rotmos_request *request);
ROTMOS_API size_t rotmos_request_header_count(const rotmos_request *request);
ROTMOS_API rotmos_status rotmos_request_header_at(const rotmos_request *request,
                                                  size_t index,
                                                  rotmos_header *out,
                                                  rotmos_error *error);
/* First lookups return EOF for absence and OK for a present empty value. */
ROTMOS_API rotmos_status
rotmos_request_header_first(const rotmos_request *request, const char *name,
                            rotmos_bytes *out, rotmos_error *error);
ROTMOS_API size_t rotmos_request_query_count(const rotmos_request *request);
ROTMOS_API rotmos_status rotmos_request_query_at(const rotmos_request *request,
                                                 size_t index,
                                                 rotmos_header *out,
                                                 rotmos_error *error);
ROTMOS_API rotmos_status
rotmos_request_query_first(const rotmos_request *request, const char *name,
                           rotmos_bytes *out, rotmos_error *error);
ROTMOS_API rotmos_status
rotmos_request_path_param(const rotmos_request *request, const char *name,
                          rotmos_bytes *out, rotmos_error *error);
ROTMOS_API rotmos_endpoint rotmos_request_peer(const rotmos_request *request);
ROTMOS_API rotmos_endpoint rotmos_request_local(const rotmos_request *request);
ROTMOS_API int rotmos_request_is_tls(const rotmos_request *request);
ROTMOS_API rotmos_status
rotmos_request_content_length(const rotmos_request *request, int *present,
                              rotmos_u64 *out, rotmos_error *error);
/* Borrows a completed in-memory body; no read-all or disk fallback. */
ROTMOS_API rotmos_status rotmos_request_body_bytes(
    const rotmos_request *request, rotmos_bytes *out, rotmos_error *error);
ROTMOS_API void *rotmos_request_userdata(const rotmos_request *request);
ROTMOS_API void rotmos_request_set_userdata(rotmos_request *request,
                                            void *userdata);

#ifdef __cplusplus
}
#endif
#endif
