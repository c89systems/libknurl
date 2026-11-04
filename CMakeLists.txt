cmake_minimum_required(VERSION 3.24)
include(cmake/OwnedPaths.cmake)
rotmos_require_owned_path("${CMAKE_BINARY_DIR}" "Build directory")
if(NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Select a concrete lifecycle toolchain target with make build or a CMake preset")
endif()
file(STRINGS VERSION rotmos_version LIMIT_COUNT 1)
project(rotmos VERSION "${rotmos_version}" LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C90_STANDARD_COMPILE_OPTION "-std=c89")
include(cmake/Dependencies.cmake)

function(rotmos_c89 target)
  set_target_properties(${target} PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO POSITION_INDEPENDENT_CODE YES C_VISIBILITY_PRESET hidden)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:C>:-std=c89;-Wstrict-prototypes;-Wmissing-prototypes>"
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
endfunction()

function(rotmos_local_runtime target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB loaders "${CMAKE_SYSROOT}/lib/ld-linux*.so*" "${CMAKE_SYSROOT}/lib/ld-musl*.so*")
    list(LENGTH loaders loader_count)
    if(NOT loader_count EQUAL 1)
      message(FATAL_ERROR "Expected one selected runtime loader in ${CMAKE_SYSROOT}/lib")
    endif()
    list(GET loaders 0 loader)
    target_link_options(${target} PRIVATE "-Wl,--dynamic-linker,${loader}" -Wl,--disable-new-dtags)
    set_target_properties(${target} PROPERTIES BUILD_RPATH
      "${CMAKE_SYSROOT}/lib;${CMAKE_SYSROOT}/usr/lib;${ROTMOS_SDK_ROOT}/cpkt/lib;${ROTMOS_SDK_ROOT}/lonejson/lib;${ROTMOS_SDK_ROOT}/pslog/lib")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(${target} PROPERTIES BUILD_RPATH
      "${ROTMOS_SDK_ROOT}/cpkt/lib;${ROTMOS_SDK_ROOT}/lonejson/lib;${ROTMOS_SDK_ROOT}/pslog/lib")
  endif()
endfunction()

include(cmake/KoreSources.cmake)
set(kore_source_dir "${CMAKE_CURRENT_BINARY_DIR}/kore")
rotmos_add_kore_sources("${CMAKE_CURRENT_SOURCE_DIR}" "${kore_source_dir}")
include(cmake/KoreBackend.cmake)

add_library(rotmos_objects OBJECT src/common.c src/u64.c src/sse.c
  src/headers.c src/source.c src/reply.c src/config_init.c src/config_copy.c
  src/config_normalize.c src/server.c src/router.c src/route_path.c
  src/request.c src/request_decode.c src/router_dispatch.c
  src/kore_runtime.c src/kore_http.c src/kore_loop.c src/kore_control.c
  src/loop.c src/loop_timer.c src/loop_waker.c
  src/response.c src/kore_response.c src/body.c src/body_materialize.c
  src/file.c src/dispatch.c src/kore_body.c src/body_reader.c
  src/channel.c src/channel_upgrade.c src/kore_channel.c
  src/tls.c src/tls_material.c src/kore_tls.c src/kore_process.c
  src/acme_store.c src/acme_files.c)
target_include_directories(rotmos_objects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(rotmos_objects PRIVATE "${kore_source_dir}/include/kore")
target_link_libraries(rotmos_objects PRIVATE OpenSSL::SSL CURL::libcurl lonejson::lonejson pslog::pslog_shared)
file(STRINGS vendor/kore.revision kore_revision LIMIT_COUNT 1)
target_compile_definitions(rotmos_objects PRIVATE ROTMOS_VERSION="${PROJECT_VERSION}"
  ROTMOS_KORE_REVISION="${kore_revision}"
  $<TARGET_PROPERTY:rotmos_kore,COMPILE_DEFINITIONS>)
add_dependencies(rotmos_objects kore-source)
rotmos_kore_object_dependencies("${kore_source_dir}/.patched"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_runtime.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_http.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_loop.c" "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_control.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_response.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_body.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_channel.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_tls.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_process.c")
rotmos_c89(rotmos_objects)
foreach(variant static shared)
  string(TOUPPER "${variant}" kind)
  add_library(rotmos_${variant} ${kind} $<TARGET_OBJECTS:rotmos_objects> $<TARGET_OBJECTS:rotmos_kore>)
  add_library(rotmos::${variant} ALIAS rotmos_${variant})
  set_target_properties(rotmos_${variant} PROPERTIES OUTPUT_NAME rotmos
    EXPORT_NAME ${variant} VERSION ${PROJECT_VERSION} SOVERSION 0)
  target_include_directories(rotmos_${variant} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  install(TARGETS rotmos_${variant} EXPORT rotmosTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endforeach()
target_link_libraries(rotmos_shared PRIVATE cpkt::openssl_ssl_shared cpkt::curl_shared
  lonejson::lonejson pslog::pslog_shared Threads::Threads)
target_link_libraries(rotmos_shared INTERFACE "$<LINK_ONLY:pslog::pslog_shared>")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(rotmos_shared PRIVATE -Wl,--no-undefined)
endif()
target_link_libraries(rotmos_static PRIVATE OpenSSL::SSL CURL::libcurl
  lonejson::lonejson_static pslog::pslog_static Threads::Threads)
set(rotmos_core_headers include/rotmos/common.h include/rotmos/http.h include/rotmos/rotmos.h
  include/rotmos/io.h include/rotmos/reply.h include/rotmos/tls.h
  include/rotmos/server.h include/rotmos/router.h include/rotmos/request.h include/rotmos/kore.h
  include/rotmos/loop.h include/rotmos/response.h include/rotmos/dispatch.h
  include/rotmos/body.h include/rotmos/channel.h)
set_source_files_properties(${rotmos_core_headers} PROPERTIES LANGUAGE C HEADER_FILE_ONLY FALSE)
add_library(rotmos_headers_c OBJECT ${rotmos_core_headers})
target_include_directories(rotmos_headers_c PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
rotmos_c89(rotmos_headers_c)
install(DIRECTORY include/rotmos DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rotmos)
install(FILES vendor/kore/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rotmos/kore)
install(FILES LICENSES/BSD-queue.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rotmos)
install(EXPORT rotmosTargets NAMESPACE rotmos:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rotmos)
configure_package_config_file(cmake/rotmosConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/rotmosConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rotmos)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/rotmosConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rotmosConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/rotmosConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rotmos)
configure_file(cmake/rotmos.pc.in "${CMAKE_CURRENT_BINARY_DIR}/rotmos.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rotmos.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

get_filename_component(rotmos_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(rotmos_can_run FALSE)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL CMAKE_SYSTEM_NAME AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL CMAKE_SYSTEM_PROCESSOR)
  set(rotmos_can_run TRUE)
endif()

if(BUILD_TESTING)
  add_executable(rotmos_acme_state tests/acme-state.c)
  target_compile_definitions(rotmos_acme_state PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_include_directories(rotmos_acme_state PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(rotmos_acme_state PRIVATE rotmos::static Threads::Threads OpenSSL::SSL)
  rotmos_c89(rotmos_acme_state)
  rotmos_local_runtime(rotmos_acme_state)
  add_test(NAME rotmos.acme-state COMMAND rotmos_acme_state)
  set_tests_properties(rotmos.acme-state PROPERTIES TIMEOUT 12)
  add_executable(rotmos_auxiliary_runtime tests/auxiliary-runtime.c)
  add_dependencies(rotmos_auxiliary_runtime kore-source)
  target_include_directories(rotmos_auxiliary_runtime PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(rotmos_auxiliary_runtime PRIVATE
    $<TARGET_PROPERTY:rotmos_kore,COMPILE_DEFINITIONS>)
  rotmos_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/auxiliary-runtime.c")
  target_link_libraries(rotmos_auxiliary_runtime PRIVATE rotmos::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  rotmos_c89(rotmos_auxiliary_runtime)
  rotmos_local_runtime(rotmos_auxiliary_runtime)
  add_test(NAME rotmos.auxiliary-runtime COMMAND rotmos_auxiliary_runtime)
  add_test(NAME rotmos.auxiliary-restart COMMAND rotmos_auxiliary_runtime restart)
  add_test(NAME rotmos.auxiliary-terminate COMMAND rotmos_auxiliary_runtime terminate)
  set_tests_properties(rotmos.auxiliary-runtime rotmos.auxiliary-restart rotmos.auxiliary-terminate PROPERTIES TIMEOUT 8)
  add_executable(rotmos_process_runtime tests/process-runtime.c)
  target_link_libraries(rotmos_process_runtime PRIVATE rotmos::shared Threads::Threads)
  rotmos_c89(rotmos_process_runtime)
  rotmos_local_runtime(rotmos_process_runtime)
  add_test(NAME rotmos.process-runtime COMMAND rotmos_process_runtime)
  set_tests_properties(rotmos.process-runtime PROPERTIES TIMEOUT 8)
  add_executable(rotmos_process_backend tests/process-runtime.c tests/process-backend.c)
  add_dependencies(rotmos_process_backend kore-source)
  target_include_directories(rotmos_process_backend PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(rotmos_process_backend PRIVATE ROTMOS_PROCESS_BACKEND_TEST=1
    $<TARGET_PROPERTY:rotmos_kore,COMPILE_DEFINITIONS>)
  rotmos_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/process-backend.c")
  target_link_libraries(rotmos_process_backend PRIVATE rotmos::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  rotmos_c89(rotmos_process_backend)
  rotmos_local_runtime(rotmos_process_backend)
  add_test(NAME rotmos.process-backend COMMAND rotmos_process_backend)
  set_tests_properties(rotmos.process-backend PROPERTIES TIMEOUT 8)
  add_executable(rotmos_tls_runtime tests/tls-runtime.c)
  target_link_libraries(rotmos_tls_runtime PRIVATE rotmos::shared cpkt::openssl_ssl_shared Threads::Threads)
  rotmos_c89(rotmos_tls_runtime)
  rotmos_local_runtime(rotmos_tls_runtime)
  add_test(NAME rotmos.tls-runtime COMMAND rotmos_tls_runtime)
  set_tests_properties(rotmos.tls-runtime PROPERTIES TIMEOUT 15)
  add_executable(rotmos_channel_runtime tests/channel-runtime.c)
  target_link_libraries(rotmos_channel_runtime PRIVATE rotmos::shared Threads::Threads)
  rotmos_c89(rotmos_channel_runtime)
  rotmos_local_runtime(rotmos_channel_runtime)
  add_test(NAME rotmos.channel-runtime COMMAND rotmos_channel_runtime)
  set_tests_properties(rotmos.channel-runtime PROPERTIES TIMEOUT 12)
  add_executable(rotmos_channel_backend tests/channel-runtime.c tests/channel-backend.c)
  add_dependencies(rotmos_channel_backend kore-source)
  target_include_directories(rotmos_channel_backend PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(rotmos_channel_backend PRIVATE ROTMOS_CHANNEL_BACKEND_TEST=1
    $<TARGET_PROPERTY:rotmos_kore,COMPILE_DEFINITIONS>)
  rotmos_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/channel-backend.c")
  target_link_libraries(rotmos_channel_backend PRIVATE rotmos::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  rotmos_c89(rotmos_channel_backend)
  rotmos_local_runtime(rotmos_channel_backend)
  add_test(NAME rotmos.channel-backend COMMAND rotmos_channel_backend)
  set_tests_properties(rotmos.channel-backend PROPERTIES TIMEOUT 12)
  add_executable(rotmos_body_runtime tests/body-runtime.c)
  target_link_libraries(rotmos_body_runtime PRIVATE rotmos::shared Threads::Threads)
  rotmos_c89(rotmos_body_runtime)
  rotmos_local_runtime(rotmos_body_runtime)
  add_test(NAME rotmos.body-runtime COMMAND rotmos_body_runtime)
  set_tests_properties(rotmos.body-runtime PROPERTIES TIMEOUT 12)
  add_executable(rotmos_response_runtime tests/response-runtime.c)
  target_link_libraries(rotmos_response_runtime PRIVATE rotmos::shared)
  rotmos_c89(rotmos_response_runtime)
  rotmos_local_runtime(rotmos_response_runtime)
  add_test(NAME rotmos.response-runtime COMMAND rotmos_response_runtime)
  set_tests_properties(rotmos.response-runtime PROPERTIES TIMEOUT 15)
  add_executable(rotmos_loop_smoke tests/loop-smoke.c)
  target_link_libraries(rotmos_loop_smoke PRIVATE rotmos::shared)
  rotmos_c89(rotmos_loop_smoke)
  rotmos_local_runtime(rotmos_loop_smoke)
  add_test(NAME rotmos.loop-smoke COMMAND rotmos_loop_smoke)
  set_tests_properties(rotmos.loop-smoke PROPERTIES TIMEOUT 10)
  add_executable(rotmos_server_runtime tests/server-runtime.c)
  target_link_libraries(rotmos_server_runtime PRIVATE rotmos::shared Threads::Threads)
  rotmos_c89(rotmos_server_runtime)
  rotmos_local_runtime(rotmos_server_runtime)
  add_test(NAME rotmos.server-runtime COMMAND rotmos_server_runtime)
  set_tests_properties(rotmos.server-runtime PROPERTIES TIMEOUT 30)
  add_executable(rotmos_kore_runtime tests/kore-runtime.c $<TARGET_OBJECTS:rotmos_kore>)
  add_dependencies(rotmos_kore_runtime kore-source)
  rotmos_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/kore-runtime.c" "${CMAKE_CURRENT_SOURCE_DIR}/tests/kore-port.c")
  target_compile_definitions(rotmos_kore_runtime PRIVATE $<TARGET_PROPERTY:rotmos_kore,COMPILE_DEFINITIONS>)
  target_include_directories(rotmos_kore_runtime PRIVATE "${kore_source_dir}/include/kore")
  target_link_libraries(rotmos_kore_runtime PRIVATE cpkt::openssl_ssl_shared cpkt::curl_shared
    lonejson::lonejson pslog::pslog_shared)
  rotmos_c89(rotmos_kore_runtime)
  rotmos_local_runtime(rotmos_kore_runtime)
  add_test(NAME rotmos.kore-runtime COMMAND rotmos_kore_runtime)
  set_tests_properties(rotmos.kore-runtime PROPERTIES TIMEOUT 20)
  add_test(NAME kore.backend-invalidation COMMAND "${CMAKE_COMMAND}"
    "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/backend invalidation"
    "-DTOOLCHAIN=${rotmos_toolchain}" "-DROTMOS_TARGET=${ROTMOS_TARGET}"
    "-DROTMOS_CAN_RUN=${rotmos_can_run}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BackendInvalidation.cmake")
  set_tests_properties(kore.backend-invalidation PROPERTIES TIMEOUT 60)
  add_executable(rotmos_declarations tests/declarations.c)
  target_link_libraries(rotmos_declarations PRIVATE rotmos::shared)
  rotmos_c89(rotmos_declarations)
  rotmos_local_runtime(rotmos_declarations)
  add_test(NAME rotmos.declarations COMMAND rotmos_declarations)
  add_executable(rotmos_reply_test tests/reply.c)
  target_link_libraries(rotmos_reply_test PRIVATE rotmos::shared)
  rotmos_c89(rotmos_reply_test)
  rotmos_local_runtime(rotmos_reply_test)
  add_test(NAME rotmos.reply COMMAND rotmos_reply_test)
  add_executable(rotmos_kore_port tests/kore-port.c "${kore_source_dir}/src/sha2.c")
  add_dependencies(rotmos_kore_port kore-source)
  target_compile_definitions(rotmos_kore_port PRIVATE _GNU_SOURCE=1 TLS_BACKEND_OPENSSL)
  target_include_directories(rotmos_kore_port PRIVATE "${kore_source_dir}/include/kore")
  target_link_libraries(rotmos_kore_port PRIVATE OpenSSL::SSL)
  rotmos_c89(rotmos_kore_port)
  rotmos_local_runtime(rotmos_kore_port)
  add_test(NAME rotmos.kore-port COMMAND rotmos_kore_port)
  add_executable(rotmos_foundation tests/foundation.c)
  target_link_libraries(rotmos_foundation PRIVATE rotmos::shared)
  rotmos_c89(rotmos_foundation)
  rotmos_local_runtime(rotmos_foundation)
  add_test(NAME rotmos.foundation COMMAND rotmos_foundation)
  add_executable(rotmos_headers_cpp tests/headers.cpp)
  target_link_libraries(rotmos_headers_cpp PRIVATE rotmos::shared)
  set_target_properties(rotmos_headers_cpp PROPERTIES CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  target_compile_options(rotmos_headers_cpp PRIVATE -Wall -Wextra -Wpedantic -Werror)
  rotmos_local_runtime(rotmos_headers_cpp)
  add_test(NAME rotmos.headers.cpp COMMAND rotmos_headers_cpp)
  find_package(Git REQUIRED)
  add_test(NAME rotmos.installed-consumer COMMAND "${CMAKE_COMMAND}"
    "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DROTMOS_BINARY=${CMAKE_CURRENT_BINARY_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/consumer"
    "-DTOOLCHAIN=${rotmos_toolchain}" "-DROTMOS_TARGET=${ROTMOS_TARGET}"
    "-DROTMOS_SDK_ROOT=${ROTMOS_SDK_ROOT}"
    "-DROTMOS_CAN_RUN=${rotmos_can_run}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/InstalledConsumer.cmake")
  set_tests_properties(rotmos.installed-consumer PROPERTIES TIMEOUT 60)
  add_test(NAME kore.patch-replay
    COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/patch-replay"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PatchReplay.cmake")
  add_test(NAME kore.preparation
    COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/preparation"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Preparation.cmake")
  add_test(NAME kore.build-invalidation
    COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/build invalidation"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildInvalidation.cmake")
  find_program(BASH_EXECUTABLE bash REQUIRED)
  add_test(NAME kore.upgrade
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/upgrade-kore.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tests/upgrade path")
  set_tests_properties(kore.patch-replay kore.preparation kore.build-invalidation
    kore.upgrade PROPERTIES TIMEOUT 60)
  if(NOT rotmos_can_run)
    set_tests_properties(rotmos.acme-state rotmos.auxiliary-runtime rotmos.auxiliary-restart rotmos.auxiliary-terminate rotmos.process-runtime rotmos.process-backend rotmos.tls-runtime rotmos.channel-runtime rotmos.channel-backend rotmos.body-runtime rotmos.response-runtime rotmos.loop-smoke rotmos.server-runtime rotmos.kore-runtime
      rotmos.declarations rotmos.reply rotmos.kore-port rotmos.foundation
      rotmos.headers.cpp PROPERTIES DISABLED TRUE)
  endif()
endif()
