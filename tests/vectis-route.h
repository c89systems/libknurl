#ifndef ROTMOS_REFERENCE_VECTIS_ROUTE_H
#define ROTMOS_REFERENCE_VECTIS_ROUTE_H
#include <rotmos/rotmos.h>

/* Test-only baseline declarations. The application/server/route wrappers own
 * only associations and type/error conversion, never HTTP or source execution.
 * Sources: Vectis vectis.h/vectis.c at
 * 6b1e263e0436687f68a7768ca55f4361a6e59e19. The source vtable transcribes the
 * host-owned lc_source protocol; no Lockd client or private dependency header
 * becomes a librotmos build input. */
typedef enum reference_status {
    REFERENCE_OK = 0,
    REFERENCE_INVALID = 1,
    REFERENCE_NOMEM = 2,
    REFERENCE_STATE = 3,
    REFERENCE_CONFLICT = 4,
    REFERENCE_NOT_IMPLEMENTED = 5,
    REFERENCE_TIMEOUT = 6
} reference_status;
typedef struct reference_error {
    reference_status code;
    int source;
    long dependency_code, http_status;
    char message[256], detail[256];
} reference_error;
struct reference_app {
    rotmos_server *server;
    void *routes;
    size_t requests, closed;
};
struct reference_request;
struct http_request;
struct http_request *reference_request_kore(struct reference_request *);
struct reference_mutable_bytes {
    void *data;
    size_t size;
};
struct reference_body_config {
    void *buffer;
    size_t buffer_size, memory_limit_bytes;
    const char *directory, *prefix;
};
struct reference_body_result {
    int kind;
    struct reference_mutable_bytes memory;
    char *path;
    size_t size;
    int owns_memory;
};
struct reference_body_spill_config {
    size_t memory_limit_bytes;
    const char *directory, *prefix;
};
struct reference_body_spill_result {
    struct reference_mutable_bytes memory;
    char *path;
    size_t size;
    int spooled_to_disk;
};
struct reference_response {
    rotmos_reply *reply;
};
typedef reference_status (*reference_route_handler)(struct reference_app *,
                                                    struct reference_request *,
                                                    struct reference_response *,
                                                    void *, reference_error *);
struct reference_route_config {
    int method;
    unsigned int methods;
    const char *path;
    int path_kind;
    struct {
        int mode;
        size_t max_bytes, memory_buffer_limit_bytes;
        int disk_spool_disabled;
    } body;
    reference_route_handler handler;
    void *userdata;
};
struct reference_source_error {
    int code;
    const char *message;
};
struct reference_source {
    size_t (*read)(struct reference_source *, void *, size_t,
                   struct reference_source_error *);
    int (*reset)(struct reference_source *, struct reference_source_error *);
    void (*close)(struct reference_source *);
    void *impl;
};
rotmos_status reference_source_borrow(struct reference_source *,
                                      rotmos_source **, rotmos_error *);
void reference_error_clear(reference_error *);
reference_status reference_error_from_rotmos(rotmos_status,
                                             const rotmos_error *,
                                             reference_error *);
rotmos_status reference_error_to_rotmos(reference_status,
                                        const reference_error *,
                                        rotmos_error *);
rotmos_status reference_request_acquire(rotmos_request *,
                                        const rotmos_upload_metadata *, void **,
                                        rotmos_error *);
void reference_request_release(void *);
int reference_request_is_closed(const struct reference_request *);
void reference_route_callbacks(struct reference_app *,
                               rotmos_server_callbacks *);
void reference_route_cleanup(struct reference_app *);
reference_status reference_route_register(struct reference_app *,
                                          const struct reference_route_config *,
                                          reference_error *);
int reference_request_method(struct reference_request *);
const char *reference_request_path(struct reference_request *);
const char *reference_request_ip(struct reference_request *);
const char *reference_request_param(struct reference_request *, const char *);
const char *reference_request_query(struct reference_request *, const char *);
const char *reference_request_header(struct reference_request *, const char *);
size_t reference_request_query_count(struct reference_request *);
int reference_request_query_at(struct reference_request *, size_t,
                               const char **, const char **);
reference_status reference_request_bytes(struct reference_request *,
                                         rotmos_bytes *, reference_error *);
void reference_body_config_init(struct reference_body_config *);
void reference_body_result_cleanup(struct reference_body_result *);
void reference_body_spill_cleanup(struct reference_body_spill_result *);
reference_status
reference_body_open_reader(const struct reference_body_result *,
                           struct reference_source **, reference_error *);
reference_status reference_request_materialize(
    struct reference_request *, const struct reference_body_config *,
    struct reference_body_result *, reference_error *);
reference_status reference_request_body_copy(struct reference_request *,
                                             struct reference_mutable_bytes *,
                                             reference_error *);
reference_status reference_request_body_spill(
    struct reference_request *, const struct reference_body_spill_config *,
    struct reference_body_spill_result *, reference_error *);
void reference_mutable_bytes_cleanup(struct reference_mutable_bytes *);
reference_status reference_response_bytes(struct reference_response *, int,
                                          const char *, rotmos_bytes,
                                          reference_error *);
struct reference_source *
reference_request_body_reader(struct reference_request *);
const char *reference_request_body_path(struct reference_request *);
int reference_request_body_is_spooled(struct reference_request *);
reference_status reference_response_status(struct reference_response *, int,
                                           reference_error *);
reference_status reference_response_header(struct reference_response *,
                                           const char *, const char *,
                                           reference_error *);
reference_status reference_response_text(struct reference_response *, int,
                                         const char *, const char *,
                                         reference_error *);
reference_status reference_response_file(struct reference_response *, int,
                                         const char *, const char *,
                                         reference_error *);
reference_status reference_response_source(struct reference_response *, int,
                                           const char *,
                                           struct reference_source *,
                                           reference_error *);
reference_status reference_response_stream_source(struct reference_response *,
                                                  int, const char *,
                                                  struct reference_source *,
                                                  reference_error *);
#endif
