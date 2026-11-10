#ifndef KNURL_LUA_H
#define KNURL_LUA_H
#include <knurl/common.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lua_State;
/** Owned asset-opening provider; open assets retain it through close. */
struct knurl_asset_provider;
/** Owned storage provider bound to one worker loop/run after first use. */
struct knurl_storage_provider;
/** Lua C-module entry point; pushes the native module on the existing host VM
 * and returns one result.
 */
KNURL_API int luaopen_knurl_native(struct lua_State *);
/** Existing VM only. Requires initialized base/package/string libraries. Does
 * not create a VM, open libraries, or take runtime ownership. */
KNURL_API knurl_status knurl_lua_preload(struct lua_State *, knurl_error *);
/** Push one opaque provider value for static_assets/webdav declarations. The
 * provider is borrowed and is never closed by the binding. owner_index is an
 * optional Lua owner kept alive by the value (0: caller retains ownership).
 * Keep the provider usable through every borrowing server's destruction.
 * Success pushes one value; failure preserves the original stack. Uses this
 * VM and initializes the native module if needed; no private core types. */
KNURL_API knurl_status
knurl_lua_push_asset_provider(struct lua_State *, struct knurl_asset_provider *,
                              int owner_index, knurl_error *);
/** Push a borrowed public storage provider with an optional retained Lua owner;
 * stack/lifetime rules match knurl_lua_push_asset_provider.
 */
KNURL_API knurl_status knurl_lua_push_storage_provider(
    struct lua_State *, struct knurl_storage_provider *, int owner_index,
    knurl_error *);
#ifdef __cplusplus
}
#endif
#endif
