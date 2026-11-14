# libknurl SDK

libknurl is a C89 HTTP server library with a public C API and a
[Kore](https://kore.io/) backend. This SDK provides public headers, static/shared
libraries, CMake/pkg-config metadata and a foreground HTTP server example.

Supply matching external SDKs for cpkt, LoneJSON and pslog. The exact versions,
target and archive hashes appear in `DEPENDENCIES`; `BACKEND` records Kore's
revision and applied patch hashes. Provider libraries are not bundled here.

CMake consumers use `find_package(knurl CONFIG REQUIRED COMPONENTS shared)`
and link `knurl::shared`; use `static` and `knurl::static` for archive linkage.
Add this prefix and the provider prefixes to CMake's selected-target search
paths. Public headers compile as C89 and C++98. JSON, XML and pslog integration
interfaces have their own public headers under `include/knurl/`.

Pkg-config consumers use `pkg-config --cflags --libs knurl` with all matching
provider metadata on `PKG_CONFIG_PATH`. Archive consumers additionally use
`--static` and select the static archives. Use the same target/compiler/runtime
closure for every provider; do not substitute host libraries in a cross build.

`examples/server.c` uses only the public API. It serves `/` on 127.0.0.1:8080;
`--check` validates declarations without binding listeners. Its CMake project
accepts `KNURL_VARIANT=static|shared` and `KNURL_DISCOVERY=cmake|pkgconfig`.
On macOS its build runtime paths include the selected `KNURL_SDK_ROOT` provider
directories, so the executable can load external shared libraries.

The Lua facade/binding is a separate install using this shared SDK, the matching
Lua providers and the host's Lua VM. The C SDK does not embed a second VM.
