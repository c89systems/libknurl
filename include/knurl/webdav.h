#ifndef KNURL_WEBDAV_H
#define KNURL_WEBDAV_H
#include <knurl/router.h>
#include <knurl/storage.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Maximum copied authorization-rejection body size: 65536 bytes. */
#define KNURL_WEBDAV_AUTH_BODY_MAX 65536u
/** Maximum copied WebDAV root principal length: 254 bytes. */
#define KNURL_WEBDAV_PRINCIPAL_MAX 254u
/** Values for webdav auth action. */
typedef enum knurl_webdav_auth_action {
    /** WebDAV authorization decision: deny. */
    KNURL_WEBDAV_DENY = 0,
    /** WebDAV authorization decision: allow. */
    KNURL_WEBDAV_ALLOW,
    /** WebDAV authorization decision: challenge. */
    KNURL_WEBDAV_CHALLENGE,
    /** WebDAV authorization decision: redirect. */
    KNURL_WEBDAV_REDIRECT
} knurl_webdav_auth_action;
/** Webdav auth request. */
typedef struct knurl_webdav_auth_request {
    /** Borrowed request handle; NULL for transport-only observations. */
    knurl_request *request;
    /** Single HTTP method; methods supplies the explicit multi-method mask when
     * nonzero.
     */
    knurl_http_method method;
    /** Borrowed mount prefix during WebDAV authorization. */
    const char *mount_path_prefix;
    /** Borrowed normalized relative resource path during WebDAV authorization.
     */
    const char *resource_path;
} knurl_webdav_auth_request;
/** Webdav auth response. */
typedef struct knurl_webdav_auth_response {
    /** Requested operation or authorization decision discriminator. */
    knurl_webdav_auth_action action;
    /** HTTP status code; zero means unknown/unset where documented by this
     * event/builder.
     */
    int status;
    /** Authorized principal text, copied before callback return. */
    const char *principal;
    /** Redirect Location value, copied before callback return. */
    const char *location;
    /** Challenge header value, copied before callback return. */
    const char *www_authenticate;
    /** Optional RFC 9110 Content-Type; malformed parameter syntax is INVALID.
     */
    const char *content_type;
    /** Request body policy, or payload bytes in a response/result declaration.
     */
    knurl_bytes body;
} knurl_webdav_auth_response;
/** Owner-loop resource authorization. Fill allow/deny/challenge/redirect and
 * optional principal/reply fields; Core copies results before return. Child
 * visibility never replaces the root principal.
 */
typedef knurl_status (*knurl_webdav_auth_fn)(const knurl_webdav_auth_request *,
                                             knurl_webdav_auth_response *,
                                             void *, knurl_error *);
/** Configuration for webdav. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_webdav_config {
    /** WebDAV mount prefix copied on registration. */
    const char *path_prefix;
    /** Borrowed public asset/storage provider; retain through borrowing router
     * destruction.
     */
    knurl_storage_provider *provider;
    /** Filesystem configuration used when no custom storage provider is
     * supplied.
     */
    knurl_storage_filesystem_config storage;
    /** Optional RFC 9110 Content-Type; malformed parameter syntax is INVALID.
     */
    const char *content_type;
    /** Copied Cache-Control header value. */
    const char *cache_control;
    /** Authentication callback/declaration, evaluated before route
     * hooks/handler.
     */
    knurl_webdav_auth_fn auth;
    /** Caller-owned WebDAV authorization context retained through router
     * destruction.
     */
    void *auth_userdata;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
    /** Optional retained route ACL. */
    knurl_acl *acl;
    /** Explicit mount body admission override; zero uses the documented
     * server/file-quota policy.
     */
    knurl_u64 max_body_bytes;
    /** Storage-operation deadline in milliseconds when explicitly enabled. */
    long operation_timeout_ms;
    /** Nonzero disables the extra storage deadline; shutdown escalation remains
     * active.
     */
    int operation_timeout_disabled;
    /** Nonzero disables the mount's required authorization policy. */
    int auth_disabled;
    /** Nonzero allows unauthorized child resources to be visible as denied
     * responses.
     */
    int conceal_disabled;
} knurl_webdav_config;
/** Initialize webdav config with its documented defaults; call before applying
 * overrides.
 */
KNURL_API void knurl_webdav_config_init(knurl_webdav_config *);
/** Initialize webdav auth response with its documented defaults; call before
 * applying overrides.
 */
KNURL_API void knurl_webdav_auth_response_init(knurl_webdav_auth_response *);
/** Copied declarations/strings; callback userdata and custom provider are
 * borrowed through router close. NULL provider declares a filesystem provider
 * using storage and the server router. Protected mounts require auth and an
 * atomic scope-capable mutable provider. Read-only mounts reject writes before
 * receiving a body. Host owns credentials, identity, packed content and VM.
 * PUT is incremental; PROPFIND depth 0/1 advances one bounded listing entry.
 * Zero operation timeout selects 30000 ms; explicit disable retains shutdown
 * escalation. Neither LOCK nor UNLOCK is implemented. */
KNURL_API knurl_status knurl_router_webdav(knurl_router *,
                                           const knurl_webdav_config *,
                                           knurl_route **, knurl_error *);
/** Register a WebDAV mount under the group prefix. Declarations are copied;
 * custom provider/auth owners remain borrowed. See knurl_router_webdav.
 */
KNURL_API knurl_status knurl_route_group_webdav(knurl_route_group *,
                                                const knurl_webdav_config *,
                                                knurl_route **, knurl_error *);
/** Borrowed copied principal from the mount's successful root authorization;
 * empty outside a WebDAV request. Child visibility checks do not replace it. */
KNURL_API knurl_bytes knurl_webdav_request_principal(const knurl_request *);
#ifdef __cplusplus
}
#endif
#endif
