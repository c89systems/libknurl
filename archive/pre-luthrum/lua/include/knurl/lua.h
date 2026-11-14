#ifndef KNURL_LUA_H
#define KNURL_LUA_H
#include <knurl/common.h>
#include <knurl/task.h>
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
/** Handwritten native adapter. work runs without Lua on a task thread;
 * complete runs on the Lua owner thread after acknowledgment and returns the
 * Lua result count. It may raise a Lua error. Descriptor is copied. */
typedef struct knurl_lua_async_operation {
    knurl_task_fn work;
    int (*complete)(struct lua_State *, void *, knurl_status,
                    const knurl_error *);
    long timeout_ms;
    int timeout_disabled;
} knurl_lua_async_operation;
/** Active yieldable Knurl route only. Roots owner_index (required userdata or
 * table owning state) until native acknowledgment; yields or raises a Lua
 * error on rejection. All state reachable by work must remain heap-owned and
 * exclusively native, except owner-thread task_call callbacks. Cancellation
 * skips complete on a disconnected request. No arbitrary Lua closures run on
 * task threads. */
KNURL_API int knurl_lua_async_run(struct lua_State *,
                                  const knurl_lua_async_operation *,
                                  void *state, int owner_index);
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
