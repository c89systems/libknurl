# libknurl

libknurl is a C89 HTTP server library for websites, REST services and APIs,
with a public C API and a Lua 5.5 facade over a [Kore](https://kore.io/) backend.
It supports GNU/musl Linux and arm64 macOS. Public headers support C89 and C++98.

Applications own handlers, schemas, authentication policy, storage providers and
the Lua VM. libknurl owns HTTP transport and the reusable server mechanisms.

## Start here

- [Features, dependencies and integration](docs/overview.md)
- [Build and development](docs/development.md)
- [C SDK and example consumers](docs/sdk.md)
- [Lua API and embedding](docs/lua.md)
- [Testing](docs/testing.md), [editor setup](docs/editors.md) and
  [release preparation](docs/releasing.md)
- [Full documentation index](docs/README.md)

The [product contract](docs/server-api-spec.md) defines behavior and ownership.
The [Vectis integration contract](docs/vectis-integration-contract.md) records
consumer compatibility and the remaining application cutover gate.

## License

libknurl is [MIT licensed](LICENSE). Kore retains its
[upstream copyright and permission notices](vendor/kore/LICENSE).
