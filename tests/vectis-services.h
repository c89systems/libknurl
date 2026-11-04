#ifndef ROTMOS_REFERENCE_VECTIS_SERVICES_H
#define ROTMOS_REFERENCE_VECTIS_SERVICES_H
#include "vectis-route.h"

/* Consumer-side value/callback translation of Vectis 6b1e263. Opaque outbound
 * proxy and WebSocket handles alias the public library handles. Contexts are
 * owned by the host and retained until server destruction; no server machinery
 * is implemented here. */
typedef struct reference_proxy_inbound {
    const rotmos_proxy_inbound *native;
    void *strings;
} reference_proxy_inbound;
typedef rotmos_proxy_outbound reference_proxy_outbound;
typedef rotmos_proxy_response reference_proxy_response;
typedef rotmos_proxy_local_response reference_proxy_local_response;
typedef reference_status (*reference_proxy_rewrite_fn)(
    const reference_proxy_inbound *, reference_proxy_outbound *, void *,
    reference_error *);
typedef reference_status (*reference_proxy_preflight_fn)(
    const reference_proxy_inbound *, reference_proxy_local_response *, void *,
    reference_error *);
typedef reference_status (*reference_proxy_response_fn)(
    reference_proxy_response *, void *, reference_error *);
typedef reference_status (*reference_proxy_error_fn)(
    const reference_error *, int, reference_proxy_local_response *, void *,
    reference_error *);
struct reference_proxy_config {
    const char *path;
    unsigned int methods;
    int path_kind;
    const char *target, *const *alternate_targets;
    size_t alternate_target_count;
    int upstream_http_version;
    long connect_timeout_ms, idle_timeout_ms, total_timeout_ms;
    size_t buffer_limit_bytes;
    const char *tls_ca_pem, *tls_client_cert_pem, *tls_client_key_pem;
    reference_proxy_rewrite_fn rewrite;
    void *rewrite_userdata;
    reference_proxy_preflight_fn preflight;
    void *preflight_userdata;
    reference_proxy_response_fn modify_response;
    void *modify_response_userdata;
    reference_proxy_error_fn on_error;
    void *on_error_userdata;
};
struct reference_proxy_binding {
    struct reference_proxy_config callbacks;
};
reference_status reference_proxy_register(struct reference_app *,
                                          const struct reference_proxy_config *,
                                          struct reference_proxy_binding *,
                                          reference_error *);
rotmos_http_method
reference_proxy_inbound_method(const reference_proxy_inbound *);
const char *reference_proxy_inbound_path(const reference_proxy_inbound *);
const char *reference_proxy_inbound_query(const reference_proxy_inbound *);
const char *reference_proxy_inbound_host(const reference_proxy_inbound *);
int reference_proxy_inbound_websocket(const reference_proxy_inbound *);
const char *reference_proxy_inbound_path_param(const reference_proxy_inbound *,
                                               const char *);
size_t reference_proxy_inbound_header_count(const reference_proxy_inbound *);
#define reference_proxy_response_status rotmos_proxy_response_status
#define reference_proxy_response_header_count rotmos_proxy_response_header_count
reference_status
reference_proxy_inbound_header_at(const reference_proxy_inbound *, size_t,
                                  const char **, const char **);
reference_status
reference_proxy_response_header_at(const reference_proxy_response *, size_t,
                                   const char **, const char **);
reference_status reference_proxy_local_respond(reference_proxy_local_response *,
                                               int, const void *, size_t,
                                               reference_error *);
reference_status
reference_proxy_local_add_header(reference_proxy_local_response *, const char *,
                                 const char *, reference_error *);
reference_status
reference_proxy_outbound_select_target(reference_proxy_outbound *, size_t,
                                       reference_error *);
reference_status reference_proxy_outbound_set_method(reference_proxy_outbound *,
                                                     rotmos_http_method,
                                                     reference_error *);
reference_status reference_proxy_outbound_set_path(reference_proxy_outbound *,
                                                   const char *,
                                                   reference_error *);
reference_status reference_proxy_outbound_set_query(reference_proxy_outbound *,
                                                    const char *,
                                                    reference_error *);
reference_status reference_proxy_outbound_set_host(reference_proxy_outbound *,
                                                   const char *,
                                                   reference_error *);
reference_status reference_proxy_outbound_add_header(reference_proxy_outbound *,
                                                     const char *, const char *,
                                                     reference_error *);
reference_status reference_proxy_outbound_set_header(reference_proxy_outbound *,
                                                     const char *, const char *,
                                                     reference_error *);
reference_status
reference_proxy_outbound_remove_header(reference_proxy_outbound *, const char *,
                                       reference_error *);
reference_status reference_proxy_response_set_status(reference_proxy_response *,
                                                     int, reference_error *);
reference_status reference_proxy_response_add_header(reference_proxy_response *,
                                                     const char *, const char *,
                                                     reference_error *);
reference_status reference_proxy_response_set_header(reference_proxy_response *,
                                                     const char *, const char *,
                                                     reference_error *);
reference_status
reference_proxy_response_remove_header(reference_proxy_response *, const char *,
                                       reference_error *);

typedef reference_status (*reference_upload_open_fn)(struct reference_app *,
                                                     struct reference_request *,
                                                     void *, void **,
                                                     reference_error *);
typedef reference_status (*reference_upload_write_fn)(
    struct reference_app *, struct reference_request *, const void *, size_t,
    void *, void *, reference_error *);
typedef reference_status (*reference_upload_finish_fn)(
    struct reference_app *, struct reference_request *,
    struct reference_response *, void *, void *, reference_error *);
typedef void (*reference_upload_close_fn)(struct reference_app *,
                                          struct reference_request *, void *,
                                          void *);
typedef reference_status (*reference_upload_reader_fn)(
    struct reference_app *, struct reference_request *,
    struct reference_source *, struct reference_response *, void *,
    reference_error *);
struct reference_upload_config {
    struct reference_route_config route;
    size_t buffer_bytes;
    reference_upload_open_fn open;
    reference_upload_write_fn write;
    reference_upload_finish_fn finish;
    reference_upload_close_fn close;
    reference_upload_reader_fn reader;
    void *userdata;
};
struct reference_upload_binding {
    struct reference_app *app;
    struct reference_upload_config config;
};
reference_status
reference_upload_register(struct reference_app *,
                          const struct reference_upload_config *,
                          struct reference_upload_binding *, reference_error *);
reference_status
reference_upload_file_register(struct reference_app *,
                               const struct reference_route_config *,
                               const char *, reference_error *);

typedef rotmos_websocket reference_websocket;
typedef void (*reference_websocket_connect_fn)(struct reference_app *,
                                               reference_websocket *, void *);
typedef void (*reference_websocket_message_fn)(struct reference_app *,
                                               reference_websocket *, int,
                                               const void *, size_t, void *);
typedef void (*reference_websocket_disconnect_fn)(struct reference_app *,
                                                  reference_websocket *,
                                                  void *);
struct reference_websocket_config {
    const char *path;
    int path_kind;
    reference_websocket_connect_fn connect;
    reference_websocket_message_fn message;
    reference_websocket_disconnect_fn disconnect;
    void *userdata;
};
struct reference_websocket_binding {
    struct reference_app *app;
    struct reference_websocket_config config;
};
reference_status reference_websocket_register(
    struct reference_app *, const struct reference_websocket_config *,
    struct reference_websocket_binding *, reference_error *);
reference_status reference_websocket_send(reference_websocket *, int,
                                          const void *, size_t,
                                          reference_error *);
reference_status reference_websocket_close(reference_websocket *,
                                           reference_error *);

struct reference_webdav_auth_request {
    struct reference_request *request;
    int method;
    const char *mount_path_prefix, *resource_path;
};
struct reference_webdav_auth_response {
    int action, status_code;
    const char *location, *www_authenticate, *content_type;
    const void *body;
    size_t body_size;
    char www_authenticate_value[256], principal[255], redirect_location[8192];
};
typedef reference_status (*reference_webdav_auth_fn)(
    const struct reference_webdav_auth_request *,
    struct reference_webdav_auth_response *, void *, reference_error *);
struct reference_webdav_config {
    const char *path_prefix;
    rotmos_storage_filesystem_config storage;
    int auth_required, conceal_unauthorized;
    reference_webdav_auth_fn auth;
    void *auth_userdata;
};
/* Allocate on the heap: the host's redirect storage is 8 KiB. */
struct reference_webdav_binding {
    struct reference_webdav_config config;
    struct reference_webdav_auth_response response;
};
reference_status
reference_webdav_register(struct reference_app *,
                          const struct reference_webdav_config *,
                          struct reference_webdav_binding *,
                          rotmos_storage_provider *, reference_error *);

/* Vectis resolves app paths and selects its durable-storage client. */
struct reference_tls_material {
    const char *path;
    struct reference_source *source;
    const void *memory;
    size_t memory_size;
};
typedef reference_status (*reference_acme_state_fn)(
    const char *runtime_directory, const char *const *domains,
    size_t domain_count, void *userdata, reference_error *);
typedef void (*reference_acme_error_fn)(const reference_error *,
                                        void *userdata);
struct reference_acme_provider {
    reference_acme_state_fn hydrate, persist;
    reference_acme_error_fn error;
    void *userdata;
};
struct reference_tls_config {
    int mode, version;
    const char *cipher_list, *bind, *domain, *const *domains;
    size_t domain_count;
    unsigned short port, http_redirect_port, presented_https_port,
        presented_http_port;
    int http_redirect_enabled, require_client_certificate;
    const char *http_redirect_bind, *acme_email, *acme_directory_url,
        *acme_state_dir;
    struct reference_tls_material bundle, certificate, private_key, ca_bundle,
        client_ca_bundle;
    struct reference_acme_provider state;
    long persist_timeout_ms;
};
struct reference_tls_binding {
    rotmos_tls_identity identity;
    rotmos_listener_config listeners[2];
    const char *manual_domain;
    struct reference_acme_provider state;
    const char *const *domains;
    size_t domain_count;
};
reference_status reference_tls_translate(const struct reference_tls_config *,
                                         rotmos_server_config *,
                                         struct reference_tls_binding *,
                                         reference_error *);
#endif
