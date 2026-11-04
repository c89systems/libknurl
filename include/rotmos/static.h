#ifndef ROTMOS_STATIC_H
#define ROTMOS_STATIC_H
#include <rotmos/asset.h>
#include <rotmos/router.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct rotmos_static_config {
    const char *path;
    const char *file_path;
    const char *root_dir;
    rotmos_asset_provider *provider;
    const char *content_type;
    const char *cache_control;
    const char *index_file;
    int index_enabled;
    const char *not_found_body;
    const char *not_found_content_type;
    rotmos_http_methods methods;
    rotmos_route_hooks hooks;
} rotmos_static_config;
/* GET/HEAD; directory indexes default disabled. Assets with a different
 * default index policy enable it explicitly. Strings/declarations are copied;
 * custom provider and callback userdata are borrowed through router close. */
ROTMOS_API void rotmos_static_config_init(rotmos_static_config *);
ROTMOS_API rotmos_status rotmos_router_static_file(rotmos_router *,
                                                   const rotmos_static_config *,
                                                   rotmos_route **,
                                                   rotmos_error *);
ROTMOS_API rotmos_status
rotmos_router_static_directory(rotmos_router *, const rotmos_static_config *,
                               rotmos_route **, rotmos_error *);
ROTMOS_API rotmos_status
rotmos_router_static_assets(rotmos_router *, const rotmos_static_config *,
                            rotmos_route **, rotmos_error *);
ROTMOS_API rotmos_status rotmos_route_group_static_file(
    rotmos_route_group *, const rotmos_static_config *, rotmos_route **,
    rotmos_error *);
ROTMOS_API rotmos_status rotmos_route_group_static_directory(
    rotmos_route_group *, const rotmos_static_config *, rotmos_route **,
    rotmos_error *);
ROTMOS_API rotmos_status rotmos_route_group_static_assets(
    rotmos_route_group *, const rotmos_static_config *, rotmos_route **,
    rotmos_error *);
ROTMOS_API const char *rotmos_static_content_type(const char *);
#ifdef __cplusplus
}
#endif
#endif
