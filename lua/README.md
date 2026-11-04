# rotmos for Lua 5.5.1

`require("rotmos")` provides the librotmos HTTP server facade. Its C binding calls
the installed public shared librotmos API. It uses the host's existing Lua VM and
the released LoneJSON/pslog interop; this source package includes no server core,
Kore, dependency sources, interpreter or duplicate runtime.

Required installed SDKs, for one concrete target:

- librotmos, matching this package's `VERSION`, shared component;
- cpkt 0.6.1, including Lua 5.5.1 and its shared link closure;
- LoneJSON 0.45.0 C SDK and its public shared Lua provider;
- pslog 0.13.0 C SDK and its public shared Lua provider.

`ROTMOS_CORE_PREFIX` names the installed librotmos prefix. `ROTMOS_SDK_ROOT`
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
  -DROTMOS_TARGET=x86_64-linux-gnu -DCMAKE_BUILD_TYPE=Debug \
  -DBUILD_TESTING=OFF \
  -DROTMOS_CORE_PREFIX=/absolute/installed/rotmos \
  -DROTMOS_SDK_ROOT=/absolute/installed/providers
cmake --build build/debug --parallel 1
cmake --install build/debug --prefix "$PWD/build/installed"
```

For other targets use `build/<target>/debug` (or `release`), with the same target's
dependency SDKs. The installed SDK includes `rotmos/lua.h` and the
`rotmos_lua` CMake package. An embedding host can call `rotmos_lua_preload(L, ...)`
on its initialized VM; its script then uses the same `require("rotmos")` facade.

Alternatively, install the release rockspec or source rock into an owned LuaRocks
tree. Select Lua 5.5.1 headers and all three required build variables explicitly:

```sh
luarocks --lua-version=5.5 --tree="$PWD/build/rocks" build \
  /absolute/lua-rotmos-VERSION-1.src.rock \
  LUA_INCDIR=/absolute/installed/providers/cpkt/include \
  ROTMOS_TARGET=x86_64-linux-gnu \
  ROTMOS_CORE_PREFIX=/absolute/installed/rotmos \
  ROTMOS_SDK_ROOT=/absolute/installed/providers
```

Replace `VERSION` with the package version. LuaRocks installs `rotmos.lua` and
`rotmos/native.so`; the SDK command above supplies the public embedding header
and CMake metadata. The binding uses shared dependency providers already
installed in those prefixes. Make them available through the target platform's
loader and Lua module search paths. Test with a Lua interpreter/embedding host
using that same target runtime; a workstation interpreter does not prove Bootlin
ABI compatibility.

`source-manifest.txt` lists source inputs; the generated `RELEASE_MANIFEST` hashes
their bytes. The archive, release rockspec and source rock have a separate
`lua-rotmos-VERSION-CHECKSUMS` inventory. These files do not authorize a release.
