# Editor documentation and checks

## Ordinary build inputs

Run `make build` for Core C/C++ work, or `make lua` for the Lua binding and its
embedding header. The latter builds Core, installs its public SDK under the
selected build tree and prepares the released Lua providers. An initial build
is required before the editor can resolve generated and dependency headers.
There is no separate editor SDK or compilation-database bootstrap.

The checked-in `.clangd` uses these ordinary databases:

| Files | Compilation database |
| --- | --- |
| Core implementation, public C headers, examples and C/C++ consumers | `build/debug/compile_commands.json` |
| `src/lua/`, `lua/include/`, `tests/lua/` | `build/debug/lua/compile_commands.json` |
| Optional parser harnesses under `tests/fuzz/` | `build/release/fuzz/compile_commands.json` |

New implementation/consumer files need to belong to a CMake target; refresh the
normal build after adding them. Files absent from the database use inferred
commands, which may have unrelated target flags. Headers inherit the context of
their including sources. Live consumer checks should use declared source files.

The last database comes from `make fuzz-build`; it is needed only when editing
those harnesses. Full clean removes all databases and generated prerequisites;
repeat the corresponding normal build to restore them. Keep `.clangd` aligned
with the producer paths when changing the build layout.

## C and C++ comments

Public declarations use Doxygen `/** ... */` comments. Put ownership, callback
thread/lifetime, units, defaults, partial-progress and error rules on the public
function, typedef or field so callers receive them through installed headers.
Implementation-only comments do not replace declaration documentation. Keep
comments attached to the declaration they describe when editing or formatting.

A useful hover check covers a function call, callback field, configuration field,
enum value and opaque typedef. For example, inspect `knurl_owned_bytes_init`,
`knurl_server_callbacks.on_parent_ready`, `knurl_xml_config.max_depth`,
`KNURL_AGAIN` and `knurl_xml_node`. Their hovers should explain the actual contract,
not just repeat a signature. Completion documentation should carry the same
function contract, including after `completionItem/resolve` when the server
returns lazy completion items.

Parser/configuration checks can be reproduced from the checkout root:

```sh
make lua
clangd --check=src/xml_mapping.c --tweaks=none
clangd --check=include/knurl/knurl.h --tweaks=none
clangd --check=lua/include/knurl/lua.h --tweaks=none
```

These commands use ordinary `.clangd` discovery. The `--check` exit status alone
does not establish that hover/completion contains comments; that requires real
`textDocument/hover` and `textDocument/completion` LSP requests or inspection in
an editor. The [verification record](verification.md) reports those separately.
`--tweaks=none` avoids the previously recorded clangd code-action self-test
limitations; it does not disable parser diagnostics or the live LSP checks.

## Lua annotations

The Lua facade's annotations reference the shipped `knurl.meta.lua` definitions,
including methods implemented dynamically in C. LuaLS discovers both files in the
source tree; installed consumers add their `share/lua/5.5` prefix to the language
server's workspace library. Metadata contains no runtime implementation and
requires no extra module at runtime. CMake and LuaRocks both install it.

C/C++ binding comments are checked with clangd; Lua annotations are interpreted
by LuaLS/EmmyLua. Test ordinary `require("knurl")` inference when changing the
facade or metadata, including dynamic userdata methods implemented in C.
