add_library(knurl_lua MODULE module.c server.c routes.c acl.c value.c)
target_link_libraries(knurl_lua PRIVATE knurl::shared cpkt::lua_shared)
target_compile_definitions(knurl_lua PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
knurl_c89(knurl_lua)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(lua_origin "@loader_path")
else()
  set(lua_origin "$ORIGIN")
  target_link_options(knurl_lua PRIVATE "-Wl,--no-undefined")
endif()
set_target_properties(knurl_lua PROPERTIES PREFIX "" OUTPUT_NAME knurl SUFFIX ".so"
  INSTALL_RPATH "${lua_origin}/../..")
install(TARGETS knurl_lua LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5"
  COMPONENT lua EXCLUDE_FROM_ALL)
