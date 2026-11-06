#ifndef ROTMOS_LUA_H
#define ROTMOS_LUA_H
#include <rotmos/common.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lua_State;
/** Owned asset-opening provider; open assets retain it through close. */
struct rotmos_asset_provider;
/** Owned storage provider bound to one worker loop/run after first use. */
struct rotmos_storage_provider;
/** Lua C-module entry point; pushes the native module on the existing host VM
 * and returns one result.
 */
ROTMOS_API int luaopen_rotmos_native(struct lua_State *);
/** Existing VM only. Requires initialized base/package/string libraries. Does
 * not create a VM, open libraries, or take runtime ownership. */
ROTMOS_API rotmos_status rotmos_lua_preload(struct lua_State *, rotmos_error *);
/** Push one opaque provider value for static_assets/webdav declarations. The
 * provider is borrowed and is never closed by the binding. owner_index is an
 * optional Lua owner kept alive by the value (0: caller retains ownership).
 * Keep the provider usable through every borrowing server's destruction.
 * Success pushes one value; failure preserves the original stack. Uses this
 * VM and initializes the native module if needed; no private core types. */
ROTMOS_API rotmos_status rotmos_lua_push_asset_provider(
    struct lua_State *, struct rotmos_asset_provider *, int owner_index,
    rotmos_error *);
/** Push a borrowed public storage provider with an optional retained Lua owner;
 * stack/lifetime rules match rotmos_lua_push_asset_provider.
 */
ROTMOS_API rotmos_status rotmos_lua_push_storage_provider(
    struct lua_State *, struct rotmos_storage_provider *, int owner_index,
    rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
