cmake_minimum_required(VERSION 3.24)
if(NOT CMAKE_TOOLCHAIN_FILE OR NOT KNURL_TARGET)
  message(FATAL_ERROR "Select the lifecycle toolchain and concrete KNURL_TARGET; use make lua")
endif()
include(../cmake/Version.cmake)
knurl_version("${CMAKE_CURRENT_SOURCE_DIR}/.." knurl_version)
project(knurl_lua VERSION "${knurl_version}" LANGUAGES C)
set(CMAKE_C90_STANDARD_COMPILE_OPTION "-std=c89")
option(BUILD_TESTING "Build repository Lua binding tests" OFF)
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
get_filename_component(knurl_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include("${knurl_root}/cmake/OwnedPaths.cmake")
knurl_require_owned_path("${CMAKE_BINARY_DIR}" "Lua build")
if(NOT KNURL_CORE_PREFIX OR NOT KNURL_SDK_ROOT)
  message(FATAL_ERROR "Provide the installed core and independent dependency SDK prefixes")
endif()
foreach(prefix "${KNURL_CORE_PREFIX}" "${KNURL_SDK_ROOT}/cpkt" "${KNURL_SDK_ROOT}/lonejson" "${KNURL_SDK_ROOT}/lonejson-lua" "${KNURL_SDK_ROOT}/pslog" "${KNURL_SDK_ROOT}/pslog-lua")
  list(APPEND CMAKE_PREFIX_PATH "${prefix}")
  list(APPEND CMAKE_FIND_ROOT_PATH "${prefix}")
endforeach()
find_package(knurl ${PROJECT_VERSION} EXACT CONFIG REQUIRED COMPONENTS shared)
find_package(Lua 5.5.1 EXACT CONFIG REQUIRED)
find_package(lonejson_lua 0.50.0 EXACT CONFIG REQUIRED)
find_package(pslog_lua 0.13.0 EXACT CONFIG REQUIRED)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/facade.inc"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/knurl.lua"
    "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/facade.inc" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Embed.cmake"
  DEPENDS knurl.lua cmake/Embed.cmake VERBATIM)
add_library(knurl_lua SHARED ../src/lua/knurl_native.c ../src/lua/value.c ../src/lua/server.c ../src/lua/config.c ../src/lua/auth.c ../src/lua/request.c ../src/lua/source.c ../src/lua/codec.c ../src/lua/xml.c ../src/lua/body.c ../src/lua/upload.c ../src/lua/reply.c ../src/lua/routes.c ../src/lua/proxy.c ../src/lua/loop.c ../src/lua/sse.c ../src/lua/websocket.c "${CMAKE_CURRENT_BINARY_DIR}/facade.inc")
add_library(knurl::lua ALIAS knurl_lua)
target_sources(knurl_lua PRIVATE ../src/lua/rest.c ../src/lua/openapi.c ../src/lua/policy.c)
target_sources(knurl_lua PRIVATE ../src/lua/tls.c)
target_sources(knurl_lua PRIVATE ../src/lua/logging.c)
target_sources(knurl_lua PRIVATE ../src/lua/providers.c)
# Link the shipped search paths directly, including with osxcross where an
# install_name_tool may be unavailable. Embedding/test hosts resolve the shared
# SDK closure; the module does not retain workstation paths.
set_target_properties(knurl_lua PROPERTIES OUTPUT_NAME native PREFIX "" SUFFIX ".so" EXPORT_NAME lua C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO C_VISIBILITY_PRESET hidden BUILD_WITH_INSTALL_RPATH TRUE)
target_compile_options(knurl_lua PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes -Wvla -Walloca)
target_compile_definitions(knurl_lua PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
target_include_directories(knurl_lua PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(knurl_lua PUBLIC knurl::shared cpkt::lua_shared PRIVATE lonejson_lua::module pslog_lua::module)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(knurl_lua PRIVATE -Wl,--no-undefined)
endif()
install(TARGETS knurl_lua EXPORT knurlLuaTargets LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5/knurl")
install(FILES knurl.lua knurl.meta.lua DESTINATION "${CMAKE_INSTALL_DATADIR}/lua/5.5")
install(FILES include/knurl/lua.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/knurl")
install(FILES ../LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/knurl-lua")
install(EXPORT knurlLuaTargets NAMESPACE knurl:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/knurl_lua")
configure_package_config_file(cmake/knurl_luaConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/knurl_luaConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/knurl_lua")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/knurl_luaConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/knurl_luaConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/knurl_luaConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/knurl_lua")

if(BUILD_TESTING)
  include("${knurl_root}/cmake/DebugTestStacks.cmake")
  if(KNURL_TARGET STREQUAL "x86_64-linux-gnu")
    add_test(NAME knurl.lua-package COMMAND "${CMAKE_COMMAND}"
      "-DKNURL_CANDIDATE_VERSION=${KNURL_CANDIDATE_VERSION}"
      "-DKNURL_ROOT=${knurl_root}" "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/package"
      -P "${knurl_root}/tests/LuaPackage.cmake")
    set_tests_properties(knurl.lua-package PROPERTIES TIMEOUT 15)
  endif()
  add_library(knurl_provider_fixture MODULE ../tests/lua/providers.c)
  set_target_properties(knurl_provider_fixture PROPERTIES PREFIX "" SUFFIX ".so"
    C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO C_VISIBILITY_PRESET hidden)
  target_compile_options(knurl_provider_fixture PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes -Wvla -Walloca -UNDEBUG)
  target_compile_definitions(knurl_provider_fixture PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(knurl_provider_fixture PRIVATE knurl::lua)
  add_library(knurl_acme_fixture MODULE ../tests/lua/acme.c ../tests/acme-ca.c)
  set_target_properties(knurl_acme_fixture PROPERTIES PREFIX "" SUFFIX ".so"
    C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO C_VISIBILITY_PRESET hidden)
  target_compile_options(knurl_acme_fixture PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes -Wvla -Walloca -UNDEBUG)
  target_compile_definitions(knurl_acme_fixture PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(knurl_acme_fixture PRIVATE knurl::lua cpkt::openssl_ssl_shared lonejson::lonejson)
  foreach(mode preload dynamic)
    add_executable(knurl_lua_${mode}_host ../tests/lua/host.c ../tests/lua/events.c)
    set_target_properties(knurl_lua_${mode}_host PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(knurl_lua_${mode}_host PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes -Wvla -Walloca -UNDEBUG)
    target_compile_definitions(knurl_lua_${mode}_host PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
    target_compile_definitions(knurl_lua_${mode}_host PRIVATE "KNURL_LONEJSON_LUA_PREFIX=\"${KNURL_SDK_ROOT}/lonejson-lua\"")
    target_compile_definitions(knurl_lua_${mode}_host PRIVATE "KNURL_PSLOG_LUA_PREFIX=\"${KNURL_SDK_ROOT}/pslog-lua\"")
    target_compile_definitions(knurl_lua_${mode}_host PRIVATE "KNURL_TEST_MODULE_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
    target_link_libraries(knurl_lua_${mode}_host PRIVATE cpkt::lua_shared cpkt::openssl_ssl_shared)
    if(mode STREQUAL "preload")
      target_link_libraries(knurl_lua_${mode}_host PRIVATE knurl::lua)
      target_compile_definitions(knurl_lua_${mode}_host PRIVATE KNURL_PRELOAD_TEST=1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      file(GLOB loaders "${CMAKE_SYSROOT}/lib/ld-linux*.so*" "${CMAKE_SYSROOT}/lib/ld-musl*.so*")
      list(LENGTH loaders loader_count)
      if(NOT loader_count EQUAL 1)
        message(FATAL_ERROR "Expected one selected target loader")
      endif()
      list(GET loaders 0 loader)
      target_link_options(knurl_lua_${mode}_host PRIVATE "-Wl,--dynamic-linker,${loader}" -Wl,--disable-new-dtags)
    endif()
    set_target_properties(knurl_lua_${mode}_host PROPERTIES BUILD_RPATH "${CMAKE_SYSROOT}/lib;${CMAKE_SYSROOT}/usr/lib;${KNURL_CORE_PREFIX}/lib;${KNURL_SDK_ROOT}/cpkt/lib;${KNURL_SDK_ROOT}/lonejson/lib;${KNURL_SDK_ROOT}/lonejson-lua/lib;${KNURL_SDK_ROOT}/pslog/lib;${KNURL_SDK_ROOT}/pslog-lua/lib;${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME knurl.lua-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/foundation.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-config-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/config.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-config-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-body-upload-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/body-upload.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-body-upload-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-routes-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/routes.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/routes"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-routes-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-providers-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/providers.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-providers-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-proxy-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/proxy.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/proxy"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-proxy-${mode} PROPERTIES TIMEOUT 10)
    foreach(failure error oom)
      add_test(NAME knurl.lua-proxy-terminal-${failure}-${mode}
        COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
          "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
          "-DSCRIPT=${knurl_root}/tests/lua/proxy-terminal-${failure}.lua"
          "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/proxy-terminal-${failure}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
      set_tests_properties(knurl.lua-proxy-terminal-${failure}-${mode} PROPERTIES TIMEOUT 10)
    endforeach()
    add_test(NAME knurl.lua-events-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/events.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/events"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-events-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-codecs-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/codecs.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/codecs"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-codecs-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-typed-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/typed.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/typed"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-typed-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-xml-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/xml.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-xml-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-rest-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/rest.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-rest-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-policy-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/policy.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-policy-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-logging-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/logging.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/logging"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-logging-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-tls-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/tls.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/tls"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-tls-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME knurl.lua-acme-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:knurl_lua_${mode}_host>"
      "-DSCRIPT=${knurl_root}/tests/lua/acme.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/acme"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(knurl.lua-acme-${mode} PROPERTIES TIMEOUT 10)
    if(NOT CMAKE_HOST_SYSTEM_NAME STREQUAL CMAKE_SYSTEM_NAME OR
        NOT CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL CMAKE_SYSTEM_PROCESSOR)
      set_tests_properties(knurl.lua-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-config-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-body-upload-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-routes-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-providers-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-proxy-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-proxy-terminal-error-${mode}
        knurl.lua-proxy-terminal-oom-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-events-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-codecs-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-typed-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-xml-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-rest-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-policy-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-logging-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-tls-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(knurl.lua-acme-${mode} PROPERTIES DISABLED TRUE)
    endif()
  endforeach()
  set(source_consumer_host "")
  if(KNURL_TARGET MATCHES "^x86_64-linux-(gnu|musl)$" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(source_consumer_host "$<TARGET_FILE:knurl_lua_dynamic_host>")
  endif()
  add_test(NAME knurl.lua-source-consumer COMMAND "${CMAKE_COMMAND}"
    "-DKNURL_ROOT=${knurl_root}" "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/rock-consumer"
    "-DKNURL_TARGET=${KNURL_TARGET}" "-DKNURL_CORE_PREFIX=${KNURL_CORE_PREFIX}"
    "-DKNURL_SDK_ROOT=${KNURL_SDK_ROOT}" "-DHOST=${source_consumer_host}"
    "-DKNURL_CANDIDATE_VERSION=${KNURL_CANDIDATE_VERSION}"
    -P "${knurl_root}/tests/LuaRockConsumer.cmake")
  set_tests_properties(knurl.lua-source-consumer PROPERTIES TIMEOUT 60)
endif()
