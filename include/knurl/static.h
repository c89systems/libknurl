#ifndef KNURL_STATIC_H
#define KNURL_STATIC_H
#include <knurl/asset.h>
#include <knurl/router.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Configuration for static. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct knurl_static_config {
    /** Resource or route path, interpreted according to the enclosing
     * operation.
     */
    const char *path;
    /** Explicit file destination/source path copied at registration; opening
     * follows the operation's admission/submission timing.
     */
    const char *file_path;
    /** Filesystem serving/storage root; confinement and direct-root policy
     * follow the owning operation.
     */
    const char *root_dir;
    /** Borrowed public asset/storage provider; retain through borrowing router
     * destruction.
     */
    knurl_asset_provider *provider;
    /** Explicit RFC 9110 Content-Type override, including valid parameters;
     * NULL selects extension/provider inference. Malformed syntax is INVALID.
     */
    const char *content_type;
    /** Copied extension overrides, ahead of instance mappings, asset metadata
     * and built-ins. An explicit content_type overrides all extension mappings.
     */
    const knurl_mime_type *mime_types;
    /** Number of entries in mime_types; zero inherits instance/default types.
     */
    size_t mime_type_count;
    /** Nonzero disables negotiated static compression; false enables streamed
     * gzip and fresh br/gzip sidecars when Accept-Encoding permits them. */
    int compression_disabled;
    /** Nonzero disables compressed sidecar lookup, preserving streamed gzip. */
    int precompressed_disabled;
    /** Copied Cache-Control header value. */
    const char *cache_control;
    /** Directory index filename; only used when indexing is enabled. */
    const char *index_file;
    /** Nonzero enables static directory indexes; use the facade's per-helper
     * default policy.
     */
    int index_enabled;
    /** Optional copied body for static 404 replies. */
    const char *not_found_body;
    /** Content type of the configured static 404 body. */
    const char *not_found_content_type;
    /** HTTP method mask; zero uses this operation's single-method/default
     * policy.
     */
    knurl_http_methods methods;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    knurl_route_hooks hooks;
    /** Optional retained route ACL. */
    knurl_acl *acl;
} knurl_static_config;
/** GET/HEAD; directory indexes default disabled. Assets with a different
 * default index policy enable it explicitly. Strings/declarations are copied;
 * custom provider and callback userdata are borrowed through router close. */
KNURL_API void knurl_static_config_init(knurl_static_config *);
/** Register a static file route. Declarations are copied, callbacks remain
 * borrowed; range/conditional delivery uses the asset engine.
 */
KNURL_API knurl_status knurl_router_static_file(knurl_router *,
                                                const knurl_static_config *,
                                                knurl_route **, knurl_error *);
/** Register a confined filesystem directory mount. Directory indexes are
 * disabled by default; no file lookup occurs during registration.
 */
KNURL_API knurl_status knurl_router_static_directory(
    knurl_router *, const knurl_static_config *, knurl_route **, knurl_error *);
/** Register a borrowed read-only asset-provider mount. Retain the provider and
 * hook userdata through router destruction.
 */
KNURL_API knurl_status knurl_router_static_assets(knurl_router *,
                                                  const knurl_static_config *,
                                                  knurl_route **,
                                                  knurl_error *);
/** Register a static file route under the group prefix. Declarations are
 * copied, callbacks remain borrowed; range/conditional delivery uses the asset
 * engine.
 */
KNURL_API knurl_status
knurl_route_group_static_file(knurl_route_group *, const knurl_static_config *,
                              knurl_route **, knurl_error *);
/** Register a confined filesystem directory mount under the group prefix.
 * Directory indexes are disabled by default; no file lookup occurs during
 * registration.
 */
KNURL_API knurl_status knurl_route_group_static_directory(
    knurl_route_group *, const knurl_static_config *, knurl_route **,
    knurl_error *);
/** Register a borrowed read-only asset-provider mount under the group prefix.
 * Retain the provider and hook userdata through router destruction.
 */
KNURL_API knurl_status knurl_route_group_static_assets(
    knurl_route_group *, const knurl_static_config *, knurl_route **,
    knurl_error *);
/** Return a built-in MIME type inferred case-insensitively from the filename's
 * final extension, or application/octet-stream. No file access or sniffing;
 * never free the result. Instance overrides use knurl_server_content_type.
 */
KNURL_API const char *knurl_static_content_type(const char *);
#ifdef __cplusplus
}
#endif
#endif
