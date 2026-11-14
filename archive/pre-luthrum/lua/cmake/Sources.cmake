# Shared by the loadable module and the statically embedded executable.
set(knurl_lua_sources)
foreach(name knurl_native value server config auth request source codec xml body
    upload reply routes proxy loop sse websocket rest openapi policy tls logging
    providers input acl async async_task curl curl_config curl_json)
  list(APPEND knurl_lua_sources "${knurl_root}/src/lua/${name}.c")
endforeach()
