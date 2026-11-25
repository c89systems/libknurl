foreach(provider pslog lonejson)
  set(${provider}_lua_source "${KNURL_SDK_ROOT}/${provider}-lua")
  knurl_require_owned_path("${${provider}_lua_source}" "${provider} Lua source")
  if(NOT EXISTS "${${provider}_lua_source}/include/${provider}.h")
    message(FATAL_ERROR "Prepare the pinned ${provider} Lua source with scripts/deps.sh ${KNURL_TARGET} lua")
  endif()
  file(SHA256 "${${provider}_lua_source}/include/${provider}.h" lua_header)
  file(SHA256 "${KNURL_SDK_ROOT}/${provider}/include/${provider}.h" sdk_header)
  if(NOT lua_header STREQUAL sdk_header)
    message(FATAL_ERROR "The ${provider} Lua source and C SDK have different public headers")
  endif()
  set(knurl_${provider}_facade "${${provider}_lua_source}/lua/${provider}/init.lua" PARENT_SCOPE)
  set(knurl_${provider}_license "${${provider}_lua_source}/LICENSE" PARENT_SCOPE)
endforeach()
set(knurl_curl_facade "${CMAKE_CURRENT_SOURCE_DIR}/curl.lua" PARENT_SCOPE)
set(knurl_smtp_facade "${CMAKE_CURRENT_SOURCE_DIR}/knurl/smtp.lua" PARENT_SCOPE)

add_library(knurl_pslog_lua OBJECT "${pslog_lua_source}/lua/src/pslog_lua.c")
set_target_properties(knurl_pslog_lua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES
  POSITION_INDEPENDENT_CODE YES)
target_link_libraries(knurl_pslog_lua PUBLIC pslog::pslog_shared cpkt::lua_shared)
target_compile_definitions(knurl_pslog_lua PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
target_compile_options(knurl_pslog_lua PRIVATE "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")
target_include_directories(knurl_pslog_lua PRIVATE "${pslog_lua_source}/include")
add_library(knurl_lonejson_lua OBJECT "${lonejson_lua_source}/src/lua/lonejson_lua.c")
set_target_properties(knurl_lonejson_lua PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES
  POSITION_INDEPENDENT_CODE YES)
target_link_libraries(knurl_lonejson_lua PUBLIC lonejson::lonejson cpkt::lua_shared)
target_compile_definitions(knurl_lonejson_lua PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
target_compile_options(knurl_lonejson_lua PRIVATE "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=.")

add_library(knurl_lua_objects OBJECT module.c server.c routes.c acl.c value.c application.c json.c logging.c
  curl.c curl_config.c curl_json.c)
target_include_directories(knurl_lua_objects PRIVATE "${pslog_lua_source}/include" "${lonejson_lua_source}/src/lua")
target_link_libraries(knurl_lua_objects PUBLIC knurl::shared cpkt::lua_shared pslog::pslog_shared
  cpkt::curl_shared lonejson::lonejson)
target_compile_definitions(knurl_lua_objects PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
knurl_c89(knurl_lua_objects)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  set(lua_origin "@loader_path")
else()
  set(lua_origin "$ORIGIN")
endif()

if(KNURL_BUILD_LUA)
  add_library(knurl_lua MODULE)
  target_link_libraries(knurl_lua PRIVATE knurl_lua_objects knurl_pslog_lua knurl_lonejson_lua)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    target_link_options(knurl_lua PRIVATE "-Wl,--no-undefined")
  endif()
  set_target_properties(knurl_lua PROPERTIES PREFIX "" OUTPUT_NAME knurl SUFFIX ".so"
    INSTALL_RPATH "${lua_origin}/../..")
  install(TARGETS knurl_lua LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5"
    COMPONENT lua EXCLUDE_FROM_ALL)
  foreach(provider pslog lonejson)
    add_library(knurl_${provider}_module MODULE)
    target_link_libraries(knurl_${provider}_module PRIVATE knurl_${provider}_lua)
    set_target_properties(knurl_${provider}_module PROPERTIES PREFIX "" OUTPUT_NAME core SUFFIX ".so"
      LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/${provider}")
    install(TARGETS knurl_${provider}_module LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5/${provider}"
      COMPONENT lua EXCLUDE_FROM_ALL)
    install(FILES "${${provider}_lua_source}/lua/${provider}/init.lua"
      DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5/${provider}" COMPONENT lua EXCLUDE_FROM_ALL)
    install(FILES "${${provider}_lua_source}/LICENSE" DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/knurl/${provider}-lua"
      COMPONENT lua EXCLUDE_FROM_ALL)
  endforeach()
  install(FILES curl.lua DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5/curl" RENAME init.lua
    COMPONENT lua EXCLUDE_FROM_ALL)
  install(FILES curl-core.lua DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5/curl" RENAME core.lua
    COMPONENT lua EXCLUDE_FROM_ALL)
  install(FILES knurl/smtp.lua DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5/knurl"
    COMPONENT lua EXCLUDE_FROM_ALL)
endif()
