# knurl for Lua 5.5.1

`require("knurl")` provides the libknurl HTTP server facade. Its C binding calls
the installed public shared libknurl API. It uses the host's existing Lua VM and
the released LoneJSON/pslog interop; this source package includes no server core,
Kore, dependency sources, interpreter or duplicate runtime.

The source archive includes the [quick starts](../README.md) and
[public guides](../docs/README.md) for offline reading.

Required installed SDKs, for one concrete target:

- libknurl, matching this package's `VERSION`, shared component;
- cpkt 0.7.0, including Lua 5.5.1 and its shared link closure;
- LoneJSON 0.47.0 C SDK and its public shared Lua provider;
- pslog 0.13.0 C SDK and its public shared Lua provider.

`KNURL_CORE_PREFIX` names the installed libknurl prefix. `KNURL_SDK_ROOT`
contains `cpkt/`, `lonejson/`, `lonejson-lua/`, `pslog/` and `pslog-lua/`.
Toolchains must already be prepared. The included selector accepts
`x86_64-linux-gnu`, `x86_64-linux-musl`, `aarch64-linux-gnu`,
`aarch64-linux-musl`, `armhf-linux-gnu`, `armhf-linux-musl` and
`arm64-apple-darwin`. It never falls back to a host Linux compiler. Bash 4.4+,
CMake 3.24+ and Ninja are build prerequisites.

From the extracted package root, build and install a binding SDK:

```sh
cmake -S lua -B build/debug -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/cmake/toolchains/selected.cmake" \
  -DKNURL_TARGET=x86_64-linux-gnu -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF \
  -DKNURL_CORE_PREFIX=/absolute/installed/knurl \
  -DKNURL_SDK_ROOT=/absolute/installed/providers
cmake --build build/debug --parallel 1
cmake --install build/debug --prefix "$PWD/build/installed"
```

For other targets use `build/<target>/debug` (or `release`), with the same target's
dependency SDKs. The installed SDK includes `knurl/lua.h` and the
`knurl_lua` CMake package. An embedding host can call `knurl_lua_preload(L, ...)`
on its initialized VM; its script then uses the same `require("knurl")` facade.

Alternatively, install the release rockspec or source rock into an owned LuaRocks
tree. Select Lua 5.5.1 headers and all three required build variables explicitly:

```sh
luarocks --lua-version=5.5 --tree="$PWD/build/rocks" build \
  /absolute/lua-knurl-VERSION-1.src.rock \
  LUA_INCDIR=/absolute/installed/providers/cpkt/include \
  KNURL_TARGET=x86_64-linux-gnu \
  KNURL_CORE_PREFIX=/absolute/installed/knurl \
  KNURL_SDK_ROOT=/absolute/installed/providers
```

Replace `VERSION` with the package version. LuaRocks installs `knurl.lua` and
`knurl/native.so`; the SDK command above supplies the public embedding header
and CMake metadata. The binding uses shared dependency providers already
installed in those prefixes. Make them available through the target platform's
loader and Lua module search paths. Test with a Lua interpreter/embedding host
using that same target runtime; a workstation interpreter does not prove Bootlin
ABI compatibility.

`source-manifest.txt` lists source inputs; the generated `RELEASE_MANIFEST` hashes
their bytes. The archive, release rockspec and source rock have a separate
`lua-knurl-VERSION-CHECKSUMS` inventory. These files do not authorize a release.

The package also ships `knurl.meta.lua` alongside the facade (LuaRocks installs
it as `knurl/meta.lua`). Add the installed `share/lua/5.5` prefix to LuaLS's
workspace library for completion and hover comments on facade and C userdata
methods. This metadata is comment-only and is never loaded by the facade.
