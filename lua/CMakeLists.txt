cmake_minimum_required(VERSION 3.24)
if(NOT CMAKE_TOOLCHAIN_FILE OR NOT ROTMOS_TARGET)
  message(FATAL_ERROR "Select the lifecycle toolchain and concrete ROTMOS_TARGET; use make lua")
endif()
file(STRINGS ../VERSION rotmos_version LIMIT_COUNT 1)
project(rotmos_lua VERSION "${rotmos_version}" LANGUAGES C)
set(CMAKE_C90_STANDARD_COMPILE_OPTION "-std=c89")
option(BUILD_TESTING "Build repository Lua binding tests" OFF)
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
get_filename_component(rotmos_root "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)
include("${rotmos_root}/cmake/OwnedPaths.cmake")
rotmos_require_owned_path("${CMAKE_BINARY_DIR}" "Lua build")
if(NOT ROTMOS_CORE_PREFIX OR NOT ROTMOS_SDK_ROOT)
  message(FATAL_ERROR "Provide the installed core and independent dependency SDK prefixes")
endif()
foreach(prefix "${ROTMOS_CORE_PREFIX}" "${ROTMOS_SDK_ROOT}/cpkt" "${ROTMOS_SDK_ROOT}/lonejson" "${ROTMOS_SDK_ROOT}/lonejson-lua" "${ROTMOS_SDK_ROOT}/pslog" "${ROTMOS_SDK_ROOT}/pslog-lua")
  list(APPEND CMAKE_PREFIX_PATH "${prefix}")
  list(APPEND CMAKE_FIND_ROOT_PATH "${prefix}")
endforeach()
find_package(rotmos ${PROJECT_VERSION} EXACT CONFIG REQUIRED COMPONENTS shared)
find_package(Lua 5.5.1 EXACT CONFIG REQUIRED)
find_package(lonejson_lua 0.46.0 EXACT CONFIG REQUIRED)
find_package(pslog_lua 0.13.0 EXACT CONFIG REQUIRED)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/facade.inc"
  COMMAND "${CMAKE_COMMAND}" "-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/rotmos.lua"
    "-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/facade.inc" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/Embed.cmake"
  DEPENDS rotmos.lua cmake/Embed.cmake VERBATIM)
add_library(rotmos_lua SHARED ../src/lua/rotmos_native.c ../src/lua/value.c ../src/lua/server.c ../src/lua/config.c ../src/lua/auth.c ../src/lua/request.c ../src/lua/source.c ../src/lua/codec.c ../src/lua/xml.c ../src/lua/body.c ../src/lua/upload.c ../src/lua/reply.c ../src/lua/routes.c ../src/lua/proxy.c ../src/lua/loop.c ../src/lua/sse.c ../src/lua/websocket.c "${CMAKE_CURRENT_BINARY_DIR}/facade.inc")
add_library(rotmos::lua ALIAS rotmos_lua)
target_sources(rotmos_lua PRIVATE ../src/lua/rest.c ../src/lua/openapi.c ../src/lua/policy.c)
target_sources(rotmos_lua PRIVATE ../src/lua/tls.c)
target_sources(rotmos_lua PRIVATE ../src/lua/logging.c)
target_sources(rotmos_lua PRIVATE ../src/lua/providers.c)
# Link the shipped search paths directly, including with osxcross where an
# install_name_tool may be unavailable. Embedding/test hosts resolve the shared
# SDK closure; the module does not retain workstation paths.
set_target_properties(rotmos_lua PROPERTIES OUTPUT_NAME native PREFIX "" SUFFIX ".so" EXPORT_NAME lua C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO C_VISIBILITY_PRESET hidden BUILD_WITH_INSTALL_RPATH TRUE)
target_compile_options(rotmos_lua PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes)
target_compile_definitions(rotmos_lua PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
target_include_directories(rotmos_lua PRIVATE "${CMAKE_CURRENT_BINARY_DIR}" PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
target_link_libraries(rotmos_lua PUBLIC rotmos::shared cpkt::lua_shared PRIVATE lonejson_lua::module pslog_lua::module)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(rotmos_lua PRIVATE -Wl,--no-undefined)
endif()
install(TARGETS rotmos_lua EXPORT rotmosLuaTargets LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}/lua/5.5/rotmos")
install(FILES rotmos.lua DESTINATION "${CMAKE_INSTALL_DATADIR}/lua/5.5")
install(FILES include/rotmos/lua.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/rotmos")
install(FILES ../LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/rotmos-lua")
install(EXPORT rotmosLuaTargets NAMESPACE rotmos:: DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rotmos_lua")
configure_package_config_file(cmake/rotmos_luaConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/rotmos_luaConfig.cmake" INSTALL_DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rotmos_lua")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/rotmos_luaConfigVersion.cmake" VERSION "${PROJECT_VERSION}" COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rotmos_luaConfig.cmake" "${CMAKE_CURRENT_BINARY_DIR}/rotmos_luaConfigVersion.cmake" DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/rotmos_lua")

if(BUILD_TESTING)
  if(ROTMOS_TARGET STREQUAL "x86_64-linux-gnu")
    add_test(NAME rotmos.lua-package COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${rotmos_root}" "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/package"
      -P "${rotmos_root}/tests/LuaPackage.cmake")
    set_tests_properties(rotmos.lua-package PROPERTIES TIMEOUT 15)
  endif()
  add_library(rotmos_provider_fixture MODULE ../tests/lua/providers.c)
  set_target_properties(rotmos_provider_fixture PROPERTIES PREFIX "" SUFFIX ".so"
    C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO C_VISIBILITY_PRESET hidden)
  target_compile_options(rotmos_provider_fixture PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes)
  target_compile_definitions(rotmos_provider_fixture PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(rotmos_provider_fixture PRIVATE rotmos::lua)
  add_library(rotmos_acme_fixture MODULE ../tests/lua/acme.c ../tests/acme-ca.c)
  set_target_properties(rotmos_acme_fixture PROPERTIES PREFIX "" SUFFIX ".so"
    C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO C_VISIBILITY_PRESET hidden)
  target_compile_options(rotmos_acme_fixture PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes)
  target_compile_definitions(rotmos_acme_fixture PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(rotmos_acme_fixture PRIVATE rotmos::lua cpkt::openssl_ssl_shared lonejson::lonejson)
  foreach(mode preload dynamic)
    add_executable(rotmos_lua_${mode}_host ../tests/lua/host.c ../tests/lua/events.c)
    set_target_properties(rotmos_lua_${mode}_host PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO)
    target_compile_options(rotmos_lua_${mode}_host PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes)
    target_compile_definitions(rotmos_lua_${mode}_host PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
    target_compile_definitions(rotmos_lua_${mode}_host PRIVATE "ROTMOS_LONEJSON_LUA_PREFIX=\"${ROTMOS_SDK_ROOT}/lonejson-lua\"")
    target_compile_definitions(rotmos_lua_${mode}_host PRIVATE "ROTMOS_PSLOG_LUA_PREFIX=\"${ROTMOS_SDK_ROOT}/pslog-lua\"")
    target_compile_definitions(rotmos_lua_${mode}_host PRIVATE "ROTMOS_TEST_MODULE_DIR=\"${CMAKE_CURRENT_BINARY_DIR}\"")
    target_link_libraries(rotmos_lua_${mode}_host PRIVATE cpkt::lua_shared cpkt::openssl_ssl_shared)
    if(mode STREQUAL "preload")
      target_link_libraries(rotmos_lua_${mode}_host PRIVATE rotmos::lua)
      target_compile_definitions(rotmos_lua_${mode}_host PRIVATE ROTMOS_PRELOAD_TEST=1)
    endif()
    if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
      file(GLOB loaders "${CMAKE_SYSROOT}/lib/ld-linux*.so*" "${CMAKE_SYSROOT}/lib/ld-musl*.so*")
      list(LENGTH loaders loader_count)
      if(NOT loader_count EQUAL 1)
        message(FATAL_ERROR "Expected one selected target loader")
      endif()
      list(GET loaders 0 loader)
      target_link_options(rotmos_lua_${mode}_host PRIVATE "-Wl,--dynamic-linker,${loader}" -Wl,--disable-new-dtags)
    endif()
    set_target_properties(rotmos_lua_${mode}_host PROPERTIES BUILD_RPATH "${CMAKE_SYSROOT}/lib;${CMAKE_SYSROOT}/usr/lib;${ROTMOS_CORE_PREFIX}/lib;${ROTMOS_SDK_ROOT}/cpkt/lib;${ROTMOS_SDK_ROOT}/lonejson/lib;${ROTMOS_SDK_ROOT}/lonejson-lua/lib;${ROTMOS_SDK_ROOT}/pslog/lib;${ROTMOS_SDK_ROOT}/pslog-lua/lib;${CMAKE_CURRENT_BINARY_DIR}")
    add_test(NAME rotmos.lua-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/foundation.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-config-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/config.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-config-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-body-upload-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/body-upload.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-body-upload-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-routes-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/routes.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/routes"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-routes-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-providers-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/providers.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-providers-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-proxy-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/proxy.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/proxy"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-proxy-${mode} PROPERTIES TIMEOUT 10)
    foreach(failure error oom)
      add_test(NAME rotmos.lua-proxy-terminal-${failure}-${mode}
        COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
          "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
          "-DSCRIPT=${rotmos_root}/tests/lua/proxy-terminal-${failure}.lua"
          "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/proxy-terminal-${failure}"
          -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
      set_tests_properties(rotmos.lua-proxy-terminal-${failure}-${mode} PROPERTIES TIMEOUT 10)
    endforeach()
    add_test(NAME rotmos.lua-events-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/events.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/events"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-events-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-codecs-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/codecs.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/codecs"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-codecs-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-typed-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/typed.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/typed"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-typed-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-xml-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/xml.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-xml-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-rest-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/rest.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-rest-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-policy-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/policy.lua" -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-policy-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-logging-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/logging.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/logging"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-logging-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-tls-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/tls.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/tls"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-tls-${mode} PROPERTIES TIMEOUT 10)
    add_test(NAME rotmos.lua-acme-${mode} COMMAND "${CMAKE_COMMAND}" "-DBINARY=${CMAKE_CURRENT_BINARY_DIR}"
      "-DPREFIX=${CMAKE_CURRENT_BINARY_DIR}/tests/installed" "-DHOST=$<TARGET_FILE:rotmos_lua_${mode}_host>"
      "-DSCRIPT=${rotmos_root}/tests/lua/acme.lua" "-DWORKSPACE=${CMAKE_CURRENT_BINARY_DIR}/tests/acme"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/RunFixture.cmake")
    set_tests_properties(rotmos.lua-acme-${mode} PROPERTIES TIMEOUT 10)
    if(NOT ROTMOS_TARGET MATCHES "^x86_64-linux-(gnu|musl)$" OR NOT CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
      set_tests_properties(rotmos.lua-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-config-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-body-upload-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-routes-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-providers-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-proxy-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-proxy-terminal-error-${mode}
        rotmos.lua-proxy-terminal-oom-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-events-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-codecs-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-typed-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-xml-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-rest-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-policy-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-logging-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-tls-${mode} PROPERTIES DISABLED TRUE)
      set_tests_properties(rotmos.lua-acme-${mode} PROPERTIES DISABLED TRUE)
    endif()
  endforeach()
  set(source_consumer_host "")
  if(ROTMOS_TARGET MATCHES "^x86_64-linux-(gnu|musl)$" AND CMAKE_HOST_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64)$")
    set(source_consumer_host "$<TARGET_FILE:rotmos_lua_dynamic_host>")
  endif()
  add_test(NAME rotmos.lua-source-consumer COMMAND "${CMAKE_COMMAND}"
    "-DROTMOS_ROOT=${rotmos_root}" "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/rock-consumer"
    "-DROTMOS_TARGET=${ROTMOS_TARGET}" "-DROTMOS_CORE_PREFIX=${ROTMOS_CORE_PREFIX}"
    "-DROTMOS_SDK_ROOT=${ROTMOS_SDK_ROOT}" "-DHOST=${source_consumer_host}"
    -P "${rotmos_root}/tests/LuaRockConsumer.cmake")
  set_tests_properties(rotmos.lua-source-consumer PROPERTIES TIMEOUT 60)
endif()
