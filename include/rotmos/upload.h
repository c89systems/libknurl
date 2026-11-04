#ifndef ROTMOS_UPLOAD_H
#define ROTMOS_UPLOAD_H
#include <rotmos/router.h>
#include <rotmos/request.h>
#ifdef __cplusplus
extern "C" {
#endif
/* Owner-loop callbacks; write consumes a positive prefix, or AGAIN with zero.
 * Request body_resume schedules another delivery. Never block these callbacks.
 * open runs only after framing/length/body-limit admission, including for an
 * empty body. Open is optional; omitting it supplies NULL state. Successful
 * open (or its omission) owns one close notification with valid request
 * metadata. On open failure the callback cleans its own partial state.
 * finish selects a staged reply before the normal after hooks run. */
typedef rotmos_status (*rotmos_upload_open_fn)(rotmos_request *, void *userdata,
                                               void **state, rotmos_error *);
typedef rotmos_status (*rotmos_upload_write_fn)(rotmos_request *, const void *,
                                                size_t length, size_t *consumed,
                                                void *state, void *userdata,
                                                rotmos_error *);
typedef rotmos_status (*rotmos_upload_finish_fn)(rotmos_request *,
                                                 rotmos_reply *, void *state,
                                                 void *userdata,
                                                 rotmos_error *);
typedef void (*rotmos_upload_close_fn)(rotmos_request *, rotmos_status outcome,
                                       void *state, void *userdata);
typedef struct rotmos_upload_stream_config {
    rotmos_http_method method;
    rotmos_http_methods methods;
    const char *path;
    rotmos_route_path_kind path_kind;
    rotmos_body_policy body;
    rotmos_route_hooks hooks;
    rotmos_upload_open_fn open;
    rotmos_upload_write_fn write;
    rotmos_upload_finish_fn finish;
    rotmos_upload_close_fn close;
    void *userdata;
} rotmos_upload_stream_config;
ROTMOS_API void rotmos_upload_stream_config_init(rotmos_upload_stream_config *);
/* Copies declarations; callback userdata remains caller-owned. Registration
 * invokes no callbacks and opens no resources. Body mode is STREAMING_UPLOAD
 * or STREAM; both use the same bounded live input delivery. */
ROTMOS_API rotmos_status rotmos_router_upload_stream(
    rotmos_router *, const rotmos_upload_stream_config *, rotmos_route **,
    rotmos_error *);
ROTMOS_API rotmos_status rotmos_route_group_upload_stream(
    rotmos_route_group *, const rotmos_upload_stream_config *, rotmos_route **,
    rotmos_error *);
/* Immutable, copied request metadata, borrowed only during a native handler.
 * Text views have trailing NUL bytes; lengths retain empty/duplicate values.
 * No live request, backend, Lua state, or request userdata enters native work.
 */
typedef struct rotmos_upload_metadata {
    rotmos_http_method method;
    rotmos_bytes method_token, version, target, raw_path, path, query,
        authority;
    rotmos_endpoint peer, local;
    rotmos_bytes effective_address, correlation_id;
    const rotmos_header *headers, *queries, *params;
    size_t header_count, query_count, param_count, listener_index;
    rotmos_u64 content_length;
    int has_content_length, tls;
} rotmos_upload_metadata;
/* Runs once on a default-stack native thread. Consume the admitted input
 * through the borrowed reader; reset, destroy and ownership transfer are
 * rejected. Manual reads block only this native thread. Cancellation is
 * cooperative via source_read/source_cancelled. Result is an independent staged
 * reply, seeded with before-hook headers and exclusively borrowed until
 * callback return. Select a result; after hooks and publication run on the
 * owner loop after ACK. Do not access Lua or retain metadata/reader/result past
 * callback return. */
typedef rotmos_status (*rotmos_upload_reader_fn)(const rotmos_upload_metadata *,
                                                 rotmos_source *reader,
                                                 rotmos_reply *result,
                                                 void *userdata,
                                                 rotmos_error *);
typedef struct rotmos_upload_reader_config {
    rotmos_http_method method;
    rotmos_http_methods methods;
    const char *path;
    rotmos_route_path_kind path_kind;
    rotmos_body_policy body;
    rotmos_route_hooks hooks;
    size_t buffer_bytes;
    rotmos_upload_reader_fn handler;
    void *userdata;
} rotmos_upload_reader_config;
typedef struct rotmos_upload_file_config {
    rotmos_http_method method;
    rotmos_http_methods methods;
    const char *path;
    rotmos_route_path_kind path_kind;
    rotmos_body_policy body;
    rotmos_route_hooks hooks;
    const char *file_path;
} rotmos_upload_file_config;
ROTMOS_API void rotmos_upload_reader_config_init(rotmos_upload_reader_config *);
ROTMOS_API void rotmos_upload_file_config_init(rotmos_upload_file_config *);
/* Bounded live queue, at most buffer_bytes (zero selects 256 KiB) and OS pipe
 * capacity. No materialization/spool or owner-loop wait/join. Caller userdata
 * stays alive until server run returns, including native cancellation ACK. */
ROTMOS_API rotmos_status rotmos_router_upload_reader(
    rotmos_router *, const rotmos_upload_reader_config *, rotmos_route **,
    rotmos_error *);
ROTMOS_API rotmos_status rotmos_route_group_upload_reader(
    rotmos_route_group *, const rotmos_upload_reader_config *, rotmos_route **,
    rotmos_error *);
/* Explicit caller artifact, opened/truncated only after admission. Incremental
 * writes and flush/close run natively; partial output remains on failure.
 * Success matches Vectis: 200 text/plain with the decimal uploaded byte count.
 */
ROTMOS_API rotmos_status
rotmos_router_upload_file(rotmos_router *, const rotmos_upload_file_config *,
                          rotmos_route **, rotmos_error *);
ROTMOS_API rotmos_status rotmos_route_group_upload_file(
    rotmos_route_group *, const rotmos_upload_file_config *, rotmos_route **,
    rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
