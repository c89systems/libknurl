# libknurl

libknurl is an embeddable C89 HTTP server library built on Kore.
Use it to serve a site, expose a typed API or embed a reverse proxy in your own
application. Your application supplies the handlers, schemas, storage and
authentication policy.

The standalone [`knurl` runner](docs/lua-config.md) runs an application from
a single Lua file. It ships in separate tarballs for x86_64 and AArch64 Linux
and Apple Silicon macOS. Native routes stay in C and each Kore worker runs
its Lua handlers on a dedicated thread. A loadable Lua module is also
available from source.
[Product direction](docs/runtime-direction.md)

- **Compose routes.** Use literal, parameter and regex routes with groups and
  middleware, and let handlers revise a staged reply before it goes out.
- **Stream requests and responses with backpressure.** Handle live uploads and
  incremental output, with explicit options for buffered and file-backed bodies.
- **Build typed APIs.** Map JSON and XML to application records, use buffered REST
  helpers and generate OpenAPI in JSON or YAML from registered routes.
- **Proxy and serve live traffic.** Forward HTTP, SSE and WebSocket connections,
  use HTTP/2 upstreams, or implement SSE and WebSocket handlers in your app.
- **Serve files and WebDAV.** Mount directories or application asset providers,
  support range and conditional requests, and back WebDAV with custom storage.
- **Protect routes.** Configure browser sessions, password and TOTP or email
  login, and machine authentication. [Native authentication](docs/auth.md)
- **Operate the server.** Configure TLS, mutual TLS and ACME, control worker
  readiness and graceful shutdown, and collect structured access logs.

The C SDK supports C89 and C++98 consumers. SDKs can be built for GNU and musl
Linux on x86_64, aarch64 and armhf, and for arm64 macOS.
[SDK delivery](docs/sdk.md)

## Hello in C

Save this as `server.c` in a new directory.

```c
#include <knurl/knurl.h>
#include <stdio.h>

static knurl_status hello(knurl_request *request, knurl_reply *reply,
                          void *data, knurl_error *error)
{
    (void)request;
    (void)data;
    return reply->text(reply, 200, "text/plain", "Hello from libknurl!\n",
                            error);
}

int main(void)
{
    knurl_server_config config;
    knurl_listener_config listener;
    knurl_route_config route;
    knurl_error error;
    knurl_server *server = NULL;
    knurl_router *router;
    knurl_status status;

    knurl_server_config_init(&config);
    knurl_listener_config_init(&listener);
    listener.bind = "127.0.0.1";
    listener.port = 8080;
    config.listeners = &listener;
    config.listener_count = config.workers.count = 1;
    config.signals.mode = KNURL_SIGNALS_LIBRARY;

    status = knurl_server_new(&config, &server, &error);
    knurl_route_config_init(&route);
    route.path = "/";
    route.method = KNURL_HTTP_GET;
    route.handler = hello;
    if (status == KNURL_OK) {
        router = server->router(server);
        status = router->route(router, &route, NULL, &error);
    }
    if (status == KNURL_OK)
        status = server->run(server, NULL, &error);
    if (status != KNURL_OK)
        fprintf(stderr, "%s\n", error.message);
    if (server != NULL)
        server->destroy(server, NULL);
    return status == KNURL_OK ? 0 : 1;
}
```

With the matching compiler and runtime, set `KNURL_PREFIX` to the extracted Core
SDK and `KNURL_SDK_ROOT` to the directory containing the `cpkt`, `lonejson` and
`pslog` prefixes. Make their shared libraries available to your system loader.
Then copy the SDK's example build file into the directory with `server.c`.
[SDK setup](docs/sdk.md)

```sh
cp "$KNURL_PREFIX/share/knurl/examples/CMakeLists.txt" .
cmake -S . -B build -DKNURL_PREFIX="$KNURL_PREFIX" -DKNURL_SDK_ROOT="$KNURL_SDK_ROOT"
cmake --build build
./build/server
```

With the server running, `curl -fsS http://127.0.0.1:8080/` prints
`Hello from libknurl!`. Stop the server with Ctrl-C.

## Guides

The C SDK includes the public guides for offline reading.
[Documentation index](docs/README.md) · [Build from source](https://github.com/c89systems/libknurl/blob/trunk/docs/development.md)

libknurl is MIT licensed. Kore retains its upstream notices.
[libknurl license](https://github.com/c89systems/libknurl/blob/trunk/LICENSE) · [Kore license](https://github.com/c89systems/libknurl/blob/trunk/vendor/kore/LICENSE)
