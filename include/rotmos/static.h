#ifndef ROTMOS_STATIC_H
#define ROTMOS_STATIC_H
#include <rotmos/asset.h>
#include <rotmos/router.h>
#ifdef __cplusplus
extern "C" {
#endif
/** Configuration for static. Use the owning operation's initializer before
 * applying overrides.
 */
typedef struct rotmos_static_config {
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
    rotmos_asset_provider *provider;
    /** Representation MIME type; NULL selects the operation's default when
     * allowed.
     */
    const char *content_type;
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
    rotmos_http_methods methods;
    /** Staged auth/before/after callbacks; all finish before transport
     * submission.
     */
    rotmos_route_hooks hooks;
} rotmos_static_config;
/** GET/HEAD; directory indexes default disabled. Assets with a different
 * default index policy enable it explicitly. Strings/declarations are copied;
 * custom provider and callback userdata are borrowed through router close. */
ROTMOS_API void rotmos_static_config_init(rotmos_static_config *);
/** Register a static file route. Declarations are copied, callbacks remain
 * borrowed; range/conditional delivery uses the asset engine.
 */
ROTMOS_API rotmos_status rotmos_router_static_file(rotmos_router *,
                                                   const rotmos_static_config *,
                                                   rotmos_route **,
                                                   rotmos_error *);
/** Register a confined filesystem directory mount. Directory indexes are
 * disabled by default; no file lookup occurs during registration.
 */
ROTMOS_API rotmos_status
rotmos_router_static_directory(rotmos_router *, const rotmos_static_config *,
                               rotmos_route **, rotmos_error *);
/** Register a borrowed read-only asset-provider mount. Retain the provider and
 * hook userdata through router destruction.
 */
ROTMOS_API rotmos_status
rotmos_router_static_assets(rotmos_router *, const rotmos_static_config *,
                            rotmos_route **, rotmos_error *);
/** Register a static file route under the group prefix. Declarations are
 * copied, callbacks remain borrowed; range/conditional delivery uses the asset
 * engine.
 */
ROTMOS_API rotmos_status rotmos_route_group_static_file(
    rotmos_route_group *, const rotmos_static_config *, rotmos_route **,
    rotmos_error *);
/** Register a confined filesystem directory mount under the group prefix.
 * Directory indexes are disabled by default; no file lookup occurs during
 * registration.
 */
ROTMOS_API rotmos_status rotmos_route_group_static_directory(
    rotmos_route_group *, const rotmos_static_config *, rotmos_route **,
    rotmos_error *);
/** Register a borrowed read-only asset-provider mount under the group prefix.
 * Retain the provider and hook userdata through router destruction.
 */
ROTMOS_API rotmos_status rotmos_route_group_static_assets(
    rotmos_route_group *, const rotmos_static_config *, rotmos_route **,
    rotmos_error *);
/** Return a static MIME type inferred from a path extension; never free the
 * returned string.
 */
ROTMOS_API const char *rotmos_static_content_type(const char *);
#ifdef __cplusplus
}
#endif
#endif
