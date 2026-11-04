#ifndef ROTMOS_WEBDAV_H
#define ROTMOS_WEBDAV_H
#include <rotmos/router.h>
#include <rotmos/storage.h>
#ifdef __cplusplus
extern "C" {
#endif
#define ROTMOS_WEBDAV_AUTH_BODY_MAX 65536u
#define ROTMOS_WEBDAV_PRINCIPAL_MAX 254u
typedef enum rotmos_webdav_auth_action {
    ROTMOS_WEBDAV_DENY = 0,
    ROTMOS_WEBDAV_ALLOW,
    ROTMOS_WEBDAV_CHALLENGE,
    ROTMOS_WEBDAV_REDIRECT
} rotmos_webdav_auth_action;
typedef struct rotmos_webdav_auth_request {
    rotmos_request *request;
    rotmos_http_method method;
    const char *mount_path_prefix, *resource_path;
} rotmos_webdav_auth_request;
typedef struct rotmos_webdav_auth_response {
    rotmos_webdav_auth_action action;
    int status;
    const char *principal, *location, *www_authenticate, *content_type;
    rotmos_bytes body;
} rotmos_webdav_auth_response;
typedef rotmos_status (*rotmos_webdav_auth_fn)(
    const rotmos_webdav_auth_request *, rotmos_webdav_auth_response *, void *,
    rotmos_error *);
typedef struct rotmos_webdav_config {
    const char *path_prefix;
    rotmos_storage_provider *provider;
    rotmos_storage_filesystem_config storage;
    const char *content_type, *cache_control;
    rotmos_webdav_auth_fn auth;
    void *auth_userdata;
    rotmos_route_hooks hooks;
    rotmos_u64 max_body_bytes;
    long operation_timeout_ms;
    int operation_timeout_disabled, auth_disabled, conceal_disabled;
} rotmos_webdav_config;
ROTMOS_API void rotmos_webdav_config_init(rotmos_webdav_config *);
ROTMOS_API void rotmos_webdav_auth_response_init(rotmos_webdav_auth_response *);
/* Copied declarations/strings; callback userdata and custom provider are
 * borrowed through router close. NULL provider declares a filesystem provider
 * using storage and the server router. Protected mounts require auth and an
 * atomic scope-capable mutable provider. Read-only mounts reject writes before
 * receiving a body. Host owns credentials, identity, packed content and VM.
 * PUT is incremental; PROPFIND depth 0/1 advances one bounded listing entry.
 * Zero operation timeout selects 30000 ms; explicit disable retains shutdown
 * escalation. Neither LOCK nor UNLOCK is implemented. */
ROTMOS_API rotmos_status rotmos_router_webdav(rotmos_router *,
                                              const rotmos_webdav_config *,
                                              rotmos_route **, rotmos_error *);
ROTMOS_API rotmos_status rotmos_route_group_webdav(rotmos_route_group *,
                                                   const rotmos_webdav_config *,
                                                   rotmos_route **,
                                                   rotmos_error *);
/* Borrowed copied principal from the mount's successful root authorization;
 * empty outside a WebDAV request. Child visibility checks do not replace it. */
ROTMOS_API rotmos_bytes rotmos_webdav_request_principal(const rotmos_request *);
#ifdef __cplusplus
}
#endif
#endif
