cmake_minimum_required(VERSION 3.24)
include(cmake/OwnedPaths.cmake)
rotmos_require_owned_path("${CMAKE_BINARY_DIR}" "Build directory")
if(ROTMOS_AFL)
  message(FATAL_ERROR "AFL instrumentation belongs to tests/fuzz, never the shipped Core SDK")
endif()
if(NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Select a concrete lifecycle toolchain target with make build or a CMake preset")
endif()
include(cmake/Version.cmake)
rotmos_version("${CMAKE_CURRENT_SOURCE_DIR}" rotmos_version)
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
  get_target_property(kind ${target} TYPE)
  if(kind STREQUAL "EXECUTABLE")
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
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
  src/json.c src/json_buffer.c src/rest.c src/rest_reply.c src/openapi.c src/openapi_json.c src/openapi_yaml.c src/json_array.c src/json_route.c src/xml.c src/xml_sax.c src/xml_mapping.c src/xml_generator.c src/xml_route.c
  src/executor.c src/native_io.c src/source_native.c src/storage.c src/storage_assets.c src/storage_filesystem.c src/storage_fs_disk.c src/storage_fs_scope.c src/asset.c src/asset_filesystem.c src/static.c src/static_reply.c src/static_policy.c src/mime.c
  src/webdav.c src/webdav_http.c src/webdav_propfind.c src/upload.c src/upload_metadata.c src/upload_reader.c src/upload_file.c
  src/autoblock.c src/logging.c src/kore_logging.c src/address.c src/policy.c src/kore_policy.c src/headers.c src/source.c src/reply.c src/config_init.c src/config_copy.c
  src/config_normalize.c src/server.c src/router.c src/route_path.c
  src/request.c src/request_decode.c src/router_dispatch.c
  src/kore_runtime.c src/kore_http.c src/kore_loop.c src/kore_control.c
  src/loop.c src/loop_timer.c src/loop_waker.c
  src/response.c src/kore_response.c src/body.c src/body_materialize.c
  src/file.c src/dispatch.c src/kore_body.c src/body_reader.c
  src/channel.c src/channel_upgrade.c src/kore_channel.c
  src/websocket.c src/websocket_frame.c src/kore_websocket.c src/websocket_route.c
  src/proxy_common.c src/proxy_framing.c src/proxy_headers.c
  src/proxy_transport.c
  src/proxy_route.c src/proxy_runtime.c src/proxy_ws_runtime.c
  src/proxy_upload.c src/proxy_http.c src/proxy_http_upstream.c src/proxy_http_wire.c
  src/proxy_url.c src/proxy_director.c src/proxy_response.c src/proxy_local.c
  src/proxy_ws_handshake.c src/proxy_ws_wire.c src/proxy_ws_rejection.c
  src/tls.c src/tls_material.c src/kore_tls.c src/kore_process.c
  src/acme_state.c src/kore_acme.c src/kore_tls_state.c)
target_include_directories(rotmos_objects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(rotmos_objects PRIVATE "${kore_source_dir}/include/kore")
target_link_libraries(rotmos_objects PRIVATE OpenSSL::SSL CURL::libcurl LibXml2::LibXml2 lonejson::lonejson pslog::pslog_shared)
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
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_websocket.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_tls.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_process.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_acme.c"
  "${CMAKE_CURRENT_SOURCE_DIR}/src/kore_tls_state.c")
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
  cpkt::libxml2_shared pslog::pslog_shared Threads::Threads)
target_link_libraries(rotmos_shared PUBLIC lonejson::lonejson)
target_link_libraries(rotmos_shared INTERFACE "$<LINK_ONLY:pslog::pslog_shared>")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(rotmos_shared PRIVATE -Wl,--no-undefined)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_options(rotmos_shared PRIVATE "LINKER:-oso_prefix,${CMAKE_BINARY_DIR}/")
endif()
target_link_libraries(rotmos_static PRIVATE OpenSSL::SSL CURL::libcurl LibXml2::LibXml2
  pslog::pslog_static Threads::Threads)
target_link_libraries(rotmos_static PUBLIC lonejson::lonejson_static)
set(rotmos_core_headers include/rotmos/common.h include/rotmos/http.h include/rotmos/rotmos.h
  include/rotmos/io.h include/rotmos/reply.h include/rotmos/tls.h
  include/rotmos/server.h include/rotmos/pslog.h include/rotmos/openapi.h include/rotmos/router.h include/rotmos/request.h include/rotmos/kore.h
  include/rotmos/loop.h include/rotmos/response.h include/rotmos/dispatch.h
  include/rotmos/body.h include/rotmos/upload.h include/rotmos/channel.h include/rotmos/websocket.h
  include/rotmos/proxy.h include/rotmos/xml.h include/rotmos/lonejson.h
  include/rotmos/asset.h include/rotmos/static.h include/rotmos/storage.h include/rotmos/webdav.h)
set_source_files_properties(${rotmos_core_headers} PROPERTIES LANGUAGE C HEADER_FILE_ONLY FALSE)
add_library(rotmos_headers_c OBJECT ${rotmos_core_headers})
target_include_directories(rotmos_headers_c PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rotmos_headers_c PRIVATE lonejson::lonejson pslog::pslog_shared)
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
include(cmake/SDK.cmake)

if(BUILD_TESTING)
  add_executable(rotmos_target_smoke tests/target-smoke.c)
  target_link_libraries(rotmos_target_smoke PRIVATE rotmos::shared)
  rotmos_c89(rotmos_target_smoke)
  rotmos_local_runtime(rotmos_target_smoke)
  add_test(NAME rotmos.target-smoke COMMAND rotmos_target_smoke)
  set_tests_properties(rotmos.target-smoke PROPERTIES TIMEOUT 10)
  add_test(NAME rotmos.sdk-package COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/SDKPackage.cmake")
  set_tests_properties(rotmos.sdk-package PROPERTIES TIMEOUT 60)
  add_executable(rotmos_vectis_typed tests/vectis-typed.c tests/vectis-forward.c tests/vectis-route.c)
  target_link_libraries(rotmos_vectis_typed PRIVATE rotmos::shared)
  target_compile_definitions(rotmos_vectis_typed PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  rotmos_c89(rotmos_vectis_typed)
  rotmos_local_runtime(rotmos_vectis_typed)
  add_test(NAME rotmos.vectis-typed COMMAND rotmos_vectis_typed)
  add_executable(rotmos_vectis_xml tests/vectis-xml-runtime.c tests/vectis-xml.c tests/vectis-route.c)
  target_link_libraries(rotmos_vectis_xml PRIVATE rotmos::shared Threads::Threads)
  target_compile_definitions(rotmos_vectis_xml PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  rotmos_c89(rotmos_vectis_xml)
  rotmos_local_runtime(rotmos_vectis_xml)
  add_test(NAME rotmos.vectis-xml COMMAND rotmos_vectis_xml)
  add_executable(rotmos_vectis_host tests/vectis-host-runtime.c tests/vectis-host.c tests/vectis-server.c tests/vectis-route.c)
  target_link_libraries(rotmos_vectis_host PRIVATE rotmos::shared pslog::pslog_shared)
  target_compile_definitions(rotmos_vectis_host PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  rotmos_c89(rotmos_vectis_host)
  rotmos_local_runtime(rotmos_vectis_host)
  add_test(NAME rotmos.vectis-host COMMAND rotmos_vectis_host)
  set_tests_properties(rotmos.vectis-host PROPERTIES TIMEOUT 10)
  add_test(NAME rotmos.public-boundary COMMAND "${CMAKE_COMMAND}"
    "-DNM=${CMAKE_NM}" "-DLIBRARY=$<TARGET_FILE:rotmos_shared>"
    "-DPLATFORM=${CMAKE_SYSTEM_NAME}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PublicBoundary.cmake")
  add_executable(rotmos_json_runtime tests/json-runtime.c tests/vectis-forward.c tests/vectis-route.c)
  target_link_libraries(rotmos_json_runtime PRIVATE rotmos::shared)
  target_compile_definitions(rotmos_json_runtime PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  rotmos_c89(rotmos_json_runtime)
  rotmos_local_runtime(rotmos_json_runtime)
  add_test(NAME rotmos.json-runtime COMMAND rotmos_json_runtime)
  add_test(NAME rotmos.json-spool-policy COMMAND rotmos_json_runtime default-directory)
  set_tests_properties(rotmos.json-runtime rotmos.json-spool-policy PROPERTIES TIMEOUT 10)
  add_executable(rotmos_autoblock tests/autoblock.c)
  target_link_libraries(rotmos_autoblock PRIVATE rotmos::static Threads::Threads)
  target_compile_definitions(rotmos_autoblock PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  rotmos_c89(rotmos_autoblock)
  rotmos_local_runtime(rotmos_autoblock)
  add_test(NAME rotmos.autoblock COMMAND rotmos_autoblock)
  set_tests_properties(rotmos.autoblock PROPERTIES TIMEOUT 5)
  add_executable(rotmos_log_backend tests/log-backend.c)
  target_include_directories(rotmos_log_backend PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(rotmos_log_backend PRIVATE $<TARGET_PROPERTY:rotmos_kore,COMPILE_DEFINITIONS>)
  target_link_libraries(rotmos_log_backend PRIVATE rotmos::static pslog::pslog_static OpenSSL::SSL CURL::libcurl)
  add_dependencies(rotmos_log_backend kore-source)
  rotmos_c89(rotmos_log_backend)
  rotmos_local_runtime(rotmos_log_backend)
  add_test(NAME rotmos.log-backend COMMAND rotmos_log_backend)
  set_tests_properties(rotmos.log-backend PROPERTIES TIMEOUT 5)
  foreach(codec filesystem-failure vectis-filesystem materialized-output vectis-services-runtime vectis-request-reply vectis-response-head vectis-body-file vectis-route-runtime vectis-config json reply-spooled xml xml-mapped xml-runtime asset mime static-runtime storage-runtime storage-deadline storage-filesystem webdav-runtime source-runtime upload-runtime rest-runtime openapi policy-runtime policy-logs-runtime)
    add_executable(rotmos_${codec} "tests/${codec}.c")
    if(codec STREQUAL "xml-runtime")
      target_sources(rotmos_${codec} PRIVATE tests/vectis-forward.c tests/vectis-route.c)
    endif()
    if(codec STREQUAL "vectis-config" OR codec STREQUAL "vectis-response-head" OR
        codec STREQUAL "policy-runtime" OR codec STREQUAL "policy-logs-runtime")
      target_sources(rotmos_${codec} PRIVATE tests/vectis-server.c)
    endif()
    if(codec STREQUAL "vectis-services-runtime")
      target_sources(rotmos_${codec} PRIVATE tests/vectis-services.c)
    endif()
    if(codec STREQUAL "openapi")
      target_sources(rotmos_${codec} PRIVATE tests/vectis-services.c tests/vectis-route.c)
    endif()
    if(codec STREQUAL "vectis-route-runtime" OR codec STREQUAL "vectis-body-file" OR codec STREQUAL "vectis-request-reply" OR codec STREQUAL "vectis-services-runtime")
      target_sources(rotmos_${codec} PRIVATE tests/vectis-route.c)
    endif()
    if(codec STREQUAL "storage-runtime" OR codec STREQUAL "filesystem-failure" OR codec STREQUAL "storage-deadline")
      # Internal native-job coverage is not an installed SDK consumer.
      target_include_directories(rotmos_${codec} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_link_libraries(rotmos_${codec} PRIVATE rotmos::static Threads::Threads OpenSSL::SSL)
    else()
      target_link_libraries(rotmos_${codec} PRIVATE rotmos::shared Threads::Threads)
    endif()
    if(codec STREQUAL "policy-runtime" OR codec STREQUAL "policy-logs-runtime" OR codec STREQUAL "vectis-config" OR codec STREQUAL "vectis-response-head")
      target_link_libraries(rotmos_${codec} PRIVATE pslog::pslog_shared)
    endif()
    target_compile_definitions(rotmos_${codec} PRIVATE
      $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
    rotmos_c89(rotmos_${codec})
    rotmos_local_runtime(rotmos_${codec})
    add_test(NAME rotmos.${codec} COMMAND rotmos_${codec})
    set_tests_properties(rotmos.${codec} PROPERTIES TIMEOUT 8)
  endforeach()
  add_test(NAME rotmos.vectis-response-head-bounded COMMAND rotmos_vectis-response-head bounded)
  add_test(NAME rotmos.vectis-response-head-plain COMMAND rotmos_vectis-response-head plain)
  set_tests_properties(rotmos.vectis-response-head-bounded rotmos.vectis-response-head-plain
    PROPERTIES TIMEOUT 8)
  add_test(NAME rotmos.rest-spill COMMAND rotmos_rest-runtime spill)
  set_tests_properties(rotmos.rest-spill PROPERTIES TIMEOUT 10)
  add_test(NAME rotmos.storage-abandon COMMAND rotmos_storage-runtime abandon)
  set_tests_properties(rotmos.storage-abandon PROPERTIES TIMEOUT 5)
  add_test(NAME rotmos.storage-filesystem-managed COMMAND rotmos_storage-filesystem managed)
  set_tests_properties(rotmos.storage-filesystem-managed PROPERTIES TIMEOUT 8)
  add_executable(rotmos_proxy_websocket tests/proxy-websocket.c)
  target_link_libraries(rotmos_proxy_websocket PRIVATE rotmos::shared cpkt::openssl_ssl_shared)
  target_compile_definitions(rotmos_proxy_websocket PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  rotmos_c89(rotmos_proxy_websocket)
  rotmos_local_runtime(rotmos_proxy_websocket)
  add_test(NAME rotmos.proxy-websocket COMMAND rotmos_proxy_websocket)
  add_test(NAME rotmos.proxy-wss COMMAND rotmos_proxy_websocket tls)
  set_tests_properties(rotmos.proxy-websocket rotmos.proxy-wss PROPERTIES TIMEOUT 10)
  add_executable(rotmos_proxy_runtime tests/proxy-runtime.c)
  target_link_libraries(rotmos_proxy_runtime PRIVATE rotmos::shared)
  target_compile_definitions(rotmos_proxy_runtime PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  rotmos_c89(rotmos_proxy_runtime)
  rotmos_local_runtime(rotmos_proxy_runtime)
  add_test(NAME rotmos.proxy-runtime COMMAND rotmos_proxy_runtime)
  set_tests_properties(rotmos.proxy-runtime PROPERTIES TIMEOUT 10)
  add_executable(rotmos_proxy_h2 tests/proxy-h2.c)
  target_link_libraries(rotmos_proxy_h2 PRIVATE rotmos::shared
    cpkt::openssl_ssl_shared cpkt::nghttp2_shared)
  target_compile_definitions(rotmos_proxy_h2 PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  rotmos_c89(rotmos_proxy_h2)
  rotmos_local_runtime(rotmos_proxy_h2)
  add_test(NAME rotmos.proxy-h2 COMMAND rotmos_proxy_h2)
  set_tests_properties(rotmos.proxy-h2 PROPERTIES TIMEOUT 8)
  foreach(surface framing headers upload http http-upstream http-wire url director response local ws-handshake ws-wire ws-rejection transport)
    string(REPLACE "-" "_" target "rotmos_proxy_${surface}")
    add_executable(${target} "tests/proxy-${surface}.c")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L
      $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
    target_link_libraries(${target} PRIVATE rotmos::static CURL::libcurl OpenSSL::SSL Threads::Threads)
    rotmos_c89(${target})
    rotmos_local_runtime(${target})
    add_test(NAME "rotmos.proxy-${surface}" COMMAND ${target})
    set_tests_properties("rotmos.proxy-${surface}" PROPERTIES TIMEOUT 10)
  endforeach()
  add_executable(rotmos_websocket_runtime tests/websocket-runtime.c)
  target_compile_definitions(rotmos_websocket_runtime PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(rotmos_websocket_runtime PRIVATE rotmos::shared
    cpkt::openssl_ssl_shared Threads::Threads)
  rotmos_c89(rotmos_websocket_runtime)
  rotmos_local_runtime(rotmos_websocket_runtime)
  add_test(NAME rotmos.websocket-runtime COMMAND rotmos_websocket_runtime)
  add_test(NAME rotmos.websocket-tls COMMAND rotmos_websocket_runtime tls)
  set_tests_properties(rotmos.websocket-runtime rotmos.websocket-tls PROPERTIES TIMEOUT 15)

  add_executable(rotmos_acme_protocol tests/acme-protocol.c tests/acme-ca.c tests/vectis-services.c tests/vectis-route.c)
  target_compile_definitions(rotmos_acme_protocol PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(rotmos_acme_protocol PRIVATE rotmos::shared
    cpkt::openssl_ssl_shared lonejson::lonejson Threads::Threads)
  rotmos_c89(rotmos_acme_protocol)
  rotmos_local_runtime(rotmos_acme_protocol)
  add_test(NAME rotmos.acme-protocol COMMAND rotmos_acme_protocol)
  add_test(NAME rotmos.acme-trust-environment COMMAND rotmos_acme_protocol environment)
  add_test(NAME rotmos.acme-trust-directory COMMAND rotmos_acme_protocol directory)
  add_test(NAME rotmos.acme-receive-limit COMMAND rotmos_acme_protocol limit)
  add_test(NAME rotmos.acme-transport-timeout COMMAND rotmos_acme_protocol timeout)
  add_test(NAME rotmos.acme-account-errors COMMAND rotmos_acme_protocol badnonce)
  set_tests_properties(rotmos.acme-protocol rotmos.acme-trust-environment
    rotmos.acme-trust-directory rotmos.acme-receive-limit
    rotmos.acme-transport-timeout rotmos.acme-account-errors PROPERTIES TIMEOUT 10)
  add_executable(rotmos_acme_runtime tests/acme-runtime.c tests/acme-backend.c)
  add_dependencies(rotmos_acme_runtime kore-source)
  target_include_directories(rotmos_acme_runtime PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(rotmos_acme_runtime PRIVATE
    $<TARGET_PROPERTY:rotmos_kore,COMPILE_DEFINITIONS>)
  rotmos_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/acme-backend.c")
  target_link_libraries(rotmos_acme_runtime PRIVATE rotmos::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  rotmos_c89(rotmos_acme_runtime)
  rotmos_local_runtime(rotmos_acme_runtime)
  add_test(NAME rotmos.acme-runtime COMMAND rotmos_acme_runtime)
  add_test(NAME rotmos.acme-persist-failure COMMAND rotmos_acme_runtime fail)
  add_test(NAME rotmos.acme-persist-timeout COMMAND rotmos_acme_runtime timeout)
  add_test(NAME rotmos.acme-pending COMMAND rotmos_acme_runtime pending)
  add_test(NAME rotmos.tls-unbound COMMAND rotmos_acme_runtime unbound)
  set_tests_properties(rotmos.acme-runtime rotmos.acme-persist-failure
    rotmos.acme-persist-timeout rotmos.acme-pending rotmos.tls-unbound PROPERTIES TIMEOUT 8)
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
  add_executable(rotmos_tls_runtime tests/tls-runtime.c tests/vectis-services.c tests/vectis-route.c)
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
  add_test(NAME rotmos.body-rate COMMAND rotmos_body_runtime rate)
  add_test(NAME rotmos.body-unlimited COMMAND rotmos_body_runtime unlimited)
  set_tests_properties(rotmos.body-runtime rotmos.body-rate rotmos.body-unlimited
    PROPERTIES TIMEOUT 12)
  add_executable(rotmos_response_runtime tests/response-runtime.c)
  target_link_libraries(rotmos_response_runtime PRIVATE rotmos::shared)
  rotmos_c89(rotmos_response_runtime)
  rotmos_local_runtime(rotmos_response_runtime)
  add_test(NAME rotmos.response-runtime COMMAND rotmos_response_runtime)
  add_test(NAME rotmos.response-minimum-queue COMMAND rotmos_response_runtime minimum)
  set_tests_properties(rotmos.response-runtime rotmos.response-minimum-queue PROPERTIES TIMEOUT 15)
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
  add_executable(rotmos_kore_request_order tests/kore-request-order.c $<TARGET_OBJECTS:rotmos_kore>)
  add_dependencies(rotmos_kore_request_order kore-source)
  rotmos_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/kore-request-order.c")
  target_compile_definitions(rotmos_kore_request_order PRIVATE $<TARGET_PROPERTY:rotmos_kore,COMPILE_DEFINITIONS>)
  target_include_directories(rotmos_kore_request_order PRIVATE "${kore_source_dir}/include/kore")
  target_link_libraries(rotmos_kore_request_order PRIVATE cpkt::openssl_ssl_shared cpkt::curl_shared
    lonejson::lonejson pslog::pslog_shared)
  rotmos_c89(rotmos_kore_request_order)
  rotmos_local_runtime(rotmos_kore_request_order)
  add_test(NAME rotmos.kore-request-order COMMAND rotmos_kore_request_order)
  set_tests_properties(rotmos.kore-request-order PROPERTIES TIMEOUT 5)
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
  add_test(NAME lifecycle.version COMMAND "${CMAKE_COMMAND}"
    "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/version"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Version.cmake")
  add_test(NAME lifecycle.failures COMMAND "${CMAKE_COMMAND}"
    "-DROTMOS_BUILD=${CMAKE_CURRENT_BINARY_DIR}"
    "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/lifecycle"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Lifecycle.cmake")
  set_tests_properties(lifecycle.version lifecycle.failures PROPERTIES TIMEOUT 15)
  foreach(contract OwnedPaths SourceInputs Testing KoreOwnership Privacy)
    add_test(NAME lifecycle.${contract} COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/${contract}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/${contract}.cmake")
    set_tests_properties(lifecycle.${contract} PROPERTIES TIMEOUT 15)
  endforeach()
  add_test(NAME lifecycle.acquisition COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/acquisition.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tests/acquisition")
  set_tests_properties(lifecycle.acquisition PROPERTIES TIMEOUT 30)
  add_test(NAME lifecycle.dependencies COMMAND bash "${CMAKE_CURRENT_SOURCE_DIR}/tests/dependencies.sh"
    "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tests/dependencies")
  set_tests_properties(lifecycle.dependencies PROPERTIES TIMEOUT 15)
  add_executable(rotmos_reply_test tests/reply.c)
  target_link_libraries(rotmos_reply_test PRIVATE rotmos::shared)
  rotmos_c89(rotmos_reply_test)
  rotmos_local_runtime(rotmos_reply_test)
  add_test(NAME rotmos.reply COMMAND rotmos_reply_test)
  add_executable(rotmos_source_file_test tests/source-file.c)
  target_link_libraries(rotmos_source_file_test PRIVATE rotmos::shared)
  rotmos_c89(rotmos_source_file_test)
  rotmos_local_runtime(rotmos_source_file_test)
  add_test(NAME rotmos.source-file COMMAND rotmos_source_file_test)
  set_tests_properties(rotmos.source-file PROPERTIES TIMEOUT 5)
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
  target_link_libraries(rotmos_headers_cpp PRIVATE rotmos::shared pslog::pslog_shared)
  set_target_properties(rotmos_headers_cpp PROPERTIES CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  target_compile_options(rotmos_headers_cpp PRIVATE -Wall -Wextra -Wpedantic -Werror)
  rotmos_local_runtime(rotmos_headers_cpp)
  add_test(NAME rotmos.headers.cpp COMMAND rotmos_headers_cpp)
  find_package(Git REQUIRED)
  add_test(NAME rotmos.installed-consumer COMMAND "${CMAKE_COMMAND}"
    "-DROTMOS_CANDIDATE_VERSION=${ROTMOS_CANDIDATE_VERSION}"
    "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DROTMOS_BINARY=${CMAKE_CURRENT_BINARY_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/consumer"
    "-DTOOLCHAIN=${rotmos_toolchain}" "-DROTMOS_TARGET=${ROTMOS_TARGET}"
    "-DROTMOS_SDK_ROOT=${ROTMOS_SDK_ROOT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/InstalledConsumer.cmake")
  set_tests_properties(rotmos.installed-consumer PROPERTIES TIMEOUT 60 FIXTURES_SETUP installed-sdk)
  foreach(variant static shared)
    add_test(NAME "rotmos.installed-consumer.${variant}" COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/consumer" "-DVARIANT=${variant}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RunConsumers.cmake")
    set_tests_properties("rotmos.installed-consumer.${variant}" PROPERTIES TIMEOUT 60
      FIXTURES_REQUIRED installed-sdk)
  endforeach()
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
    # rotmos.* checks execute selected-target programs, except these host-tool
    # gates. Keep new runtime cases from silently escaping cross-target gating.
    get_property(runtime_tests DIRECTORY PROPERTY TESTS)
    list(FILTER runtime_tests INCLUDE REGEX "^rotmos\\.")
    list(REMOVE_ITEM runtime_tests rotmos.public-boundary rotmos.installed-consumer
      rotmos.sdk-package)
    set_tests_properties(${runtime_tests} PROPERTIES DISABLED TRUE)
  endif()
endif()
