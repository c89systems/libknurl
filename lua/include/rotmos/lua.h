#ifndef ROTMOS_LUA_H
#define ROTMOS_LUA_H
#include <rotmos/common.h>
#ifdef __cplusplus
extern "C" {
#endif
struct lua_State;
ROTMOS_API int luaopen_rotmos_native(struct lua_State *);
/* Existing VM only. Requires initialized base/package/string libraries. Does
 * not create a VM, open libraries, or take runtime ownership. */
ROTMOS_API rotmos_status rotmos_lua_preload(struct lua_State *, rotmos_error *);
#ifdef __cplusplus
}
#endif
#endif
