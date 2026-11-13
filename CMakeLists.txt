cmake_minimum_required(VERSION 3.24)
include(cmake/OwnedPaths.cmake)
knurl_require_owned_path("${CMAKE_BINARY_DIR}" "Build directory")
if(KNURL_AFL)
  message(FATAL_ERROR "AFL instrumentation belongs to tests/fuzz, never the shipped Core SDK")
endif()
if(NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Select a concrete lifecycle toolchain target with make build or a CMake preset")
endif()
include(cmake/Version.cmake)
knurl_version("${CMAKE_CURRENT_SOURCE_DIR}" knurl_version)
project(knurl VERSION "${knurl_version}" LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C90_STANDARD_COMPILE_OPTION "-std=c89")
include(cmake/Dependencies.cmake)

function(knurl_c89 target)
  set_target_properties(${target} PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO POSITION_INDEPENDENT_CODE YES C_VISIBILITY_PRESET hidden)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:C>:-std=c89;-Wstrict-prototypes;-Wmissing-prototypes;-Wvla;-Walloca>"
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
  get_target_property(kind ${target} TYPE)
  if(kind STREQUAL "EXECUTABLE")
    target_compile_options(${target} PRIVATE -UNDEBUG)
  endif()
endfunction()

function(knurl_local_runtime target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB loaders "${CMAKE_SYSROOT}/lib/ld-linux*.so*" "${CMAKE_SYSROOT}/lib/ld-musl*.so*")
    list(LENGTH loaders loader_count)
    if(NOT loader_count EQUAL 1)
      message(FATAL_ERROR "Expected one selected runtime loader in ${CMAKE_SYSROOT}/lib")
    endif()
    list(GET loaders 0 loader)
    target_link_options(${target} PRIVATE "-Wl,--dynamic-linker,${loader}" -Wl,--disable-new-dtags)
    set_target_properties(${target} PROPERTIES BUILD_RPATH
      "${CMAKE_SYSROOT}/lib;${CMAKE_SYSROOT}/usr/lib;${KNURL_SDK_ROOT}/cpkt/lib;${KNURL_SDK_ROOT}/lonejson/lib;${KNURL_SDK_ROOT}/pslog/lib")
  elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
    set_target_properties(${target} PROPERTIES BUILD_RPATH
      "${KNURL_SDK_ROOT}/cpkt/lib;${KNURL_SDK_ROOT}/lonejson/lib;${KNURL_SDK_ROOT}/pslog/lib")
  endif()
endfunction()

include(cmake/KoreSources.cmake)
set(kore_source_dir "${CMAKE_CURRENT_BINARY_DIR}/kore")
knurl_add_kore_sources("${CMAKE_CURRENT_SOURCE_DIR}" "${kore_source_dir}")
include(cmake/KoreBackend.cmake)

add_library(knurl_objects OBJECT src/common.c src/u64.c src/input.c src/input_request.c src/sse.c
  src/json.c src/json_buffer.c src/rest.c src/rest_reply.c src/openapi.c src/openapi_json.c src/openapi_yaml.c src/json_array.c src/json_route.c src/xml.c src/xml_sax.c src/xml_mapping.c src/xml_generator.c src/xml_route.c
  src/executor.c src/native_io.c src/source_native.c src/storage.c src/storage_assets.c src/storage_filesystem.c src/storage_fs_disk.c src/storage_fs_scope.c src/asset.c src/asset_filesystem.c src/static.c src/static_reply.c src/static_policy.c src/static_multipart.c src/mime.c
  src/webdav.c src/webdav_http.c src/webdav_propfind.c src/upload.c src/upload_metadata.c src/upload_reader.c src/upload_file.c
  src/autoblock.c src/logging.c src/access_log_file.c src/kore_logging.c src/address.c src/policy.c src/kore_policy.c src/headers.c src/source.c src/reply.c src/config_init.c src/config_copy.c
  src/config_normalize.c src/server.c src/router.c src/route_path.c
  src/request.c src/request_decode.c src/router_dispatch.c
  src/kore_runtime.c src/kore_http.c src/kore_loop.c src/kore_control.c
  src/loop.c src/loop_timer.c src/loop_waker.c
  src/response.c src/kore_response.c src/body.c src/body_materialize.c
  src/file.c src/dispatch.c src/kore_body.c src/body_reader.c
  src/channel.c src/channel_upgrade.c src/kore_channel.c
  src/websocket.c src/websocket_frame.c src/websocket_protocol.c src/kore_websocket.c src/websocket_route.c
  src/proxy_common.c src/http_framing.c src/proxy_headers.c
  src/proxy_transport.c
  src/proxy_route.c src/proxy_runtime.c src/proxy_ws_runtime.c
  src/proxy_upload.c src/proxy_http.c src/proxy_http_upstream.c src/proxy_http_wire.c
  src/proxy_url.c src/proxy_director.c src/proxy_response.c src/proxy_local.c
  src/proxy_ws_handshake.c src/proxy_ws_wire.c src/proxy_ws_rejection.c
  src/tls.c src/tls_material.c src/kore_tls.c src/kore_process.c
  src/acme_state.c src/kore_acme.c src/kore_tls_state.c)
target_sources(knurl_objects PRIVATE src/static_gzip.c src/static_encoding.c)
target_sources(knurl_objects PRIVATE src/mmdb.c src/mmdb_decode.c src/mmdb_file.c src/mmdb_cache.c)
target_sources(knurl_objects PRIVATE src/mmdb_prefix.c)
target_include_directories(knurl_objects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(knurl_objects PRIVATE "${kore_source_dir}/include/kore")
target_link_libraries(knurl_objects PRIVATE OpenSSL::SSL CURL::libcurl LibXml2::LibXml2 lonejson::lonejson pslog::pslog_shared)
target_link_libraries(knurl_objects PRIVATE ZLIB::ZLIB)
file(STRINGS vendor/kore.revision kore_revision LIMIT_COUNT 1)
target_compile_definitions(knurl_objects PRIVATE KNURL_VERSION="${PROJECT_VERSION}"
  KNURL_KORE_REVISION="${kore_revision}"
  $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
add_dependencies(knurl_objects kore-source)
knurl_kore_object_dependencies("${kore_source_dir}/.patched"
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
knurl_c89(knurl_objects)
set(KNURL_ABI_VERSION 0)
foreach(variant static shared)
  string(TOUPPER "${variant}" kind)
  add_library(knurl_${variant} ${kind} $<TARGET_OBJECTS:knurl_objects> $<TARGET_OBJECTS:knurl_kore>)
  add_library(knurl::${variant} ALIAS knurl_${variant})
  set_target_properties(knurl_${variant} PROPERTIES OUTPUT_NAME knurl
    EXPORT_NAME ${variant} VERSION ${PROJECT_VERSION} SOVERSION ${KNURL_ABI_VERSION}
    MACHO_COMPATIBILITY_VERSION "${KNURL_ABI_VERSION}.0.0"
    MACHO_CURRENT_VERSION "${KNURL_ABI_VERSION}.0.0")
  target_include_directories(knurl_${variant} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  install(TARGETS knurl_${variant} EXPORT knurlTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endforeach()
target_link_libraries(knurl_shared PRIVATE cpkt::openssl_ssl_shared cpkt::curl_shared
  cpkt::libxml2_shared pslog::pslog_shared Threads::Threads)
target_link_libraries(knurl_shared PUBLIC lonejson::lonejson)
target_link_libraries(knurl_shared PRIVATE cpkt::zlib_shared)
target_link_libraries(knurl_shared INTERFACE "$<LINK_ONLY:pslog::pslog_shared>")
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_link_options(knurl_shared PRIVATE -Wl,--no-undefined)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_link_options(knurl_shared PRIVATE "LINKER:-oso_prefix,${CMAKE_BINARY_DIR}/")
endif()
target_link_libraries(knurl_static PRIVATE OpenSSL::SSL CURL::libcurl LibXml2::LibXml2
  pslog::pslog_static Threads::Threads)
target_link_libraries(knurl_static PUBLIC lonejson::lonejson_static)
target_link_libraries(knurl_static PRIVATE ZLIB::ZLIB)
set(knurl_core_headers include/knurl/mmdb.h include/knurl/common.h include/knurl/input.h include/knurl/http.h include/knurl/knurl.h
  include/knurl/io.h include/knurl/reply.h include/knurl/tls.h
  include/knurl/server.h include/knurl/pslog.h include/knurl/openapi.h include/knurl/router.h include/knurl/request.h include/knurl/kore.h
  include/knurl/loop.h include/knurl/response.h include/knurl/dispatch.h
  include/knurl/body.h include/knurl/upload.h include/knurl/channel.h include/knurl/websocket.h
  include/knurl/proxy.h include/knurl/xml.h include/knurl/lonejson.h
  include/knurl/asset.h include/knurl/static.h include/knurl/storage.h include/knurl/webdav.h)
set_source_files_properties(${knurl_core_headers} PROPERTIES LANGUAGE C HEADER_FILE_ONLY FALSE)
add_library(knurl_headers_c OBJECT ${knurl_core_headers})
target_include_directories(knurl_headers_c PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(knurl_headers_c PRIVATE lonejson::lonejson pslog::pslog_shared)
knurl_c89(knurl_headers_c)
install(DIRECTORY include/knurl DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/knurl)
install(FILES vendor/kore/LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/knurl/kore)
install(FILES LICENSES/BSD-queue.txt DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/knurl)
install(EXPORT knurlTargets NAMESPACE knurl:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/knurl)
configure_package_config_file(cmake/knurlConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/knurlConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/knurl)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/knurlConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/knurlConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/knurlConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/knurl)
configure_file(cmake/knurl.pc.in "${CMAKE_CURRENT_BINARY_DIR}/knurl.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/knurl.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

get_filename_component(knurl_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(knurl_can_run FALSE)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL CMAKE_SYSTEM_NAME AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL CMAKE_SYSTEM_PROCESSOR)
  set(knurl_can_run TRUE)
endif()
include(cmake/SDK.cmake)

if(BUILD_TESTING)
  include(cmake/MMDBTests.cmake)
  add_test(NAME knurl.receiver-surface COMMAND "${CMAKE_COMMAND}"
    "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/ReceiverSurface.cmake")
  include(cmake/DebugTestStacks.cmake)
  add_test(NAME lifecycle.stack-allocation-policy COMMAND "${CMAKE_COMMAND}"
    "-DCOMPILER=${CMAKE_C_COMPILER}" "-DBUILD=${CMAKE_CURRENT_BINARY_DIR}"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/tests/stack-allocation-policy"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/StackAllocationPolicy.cmake")
  set_tests_properties(lifecycle.stack-allocation-policy PROPERTIES TIMEOUT 15)
  if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND KNURL_TARGET MATCHES "-linux-gnu$")
    foreach(kind constrained control)
      add_executable(knurl_thread_stacks_${kind} tests/thread-stacks.c)
      target_compile_definitions(knurl_thread_stacks_${kind} PRIVATE _GNU_SOURCE=1)
      target_link_libraries(knurl_thread_stacks_${kind} PRIVATE Threads::Threads)
      knurl_c89(knurl_thread_stacks_${kind})
      knurl_local_runtime(knurl_thread_stacks_${kind})
    endforeach()
    set_property(TARGET knurl_thread_stacks_control PROPERTY KNURL_STACK_CONTROL TRUE)
    target_compile_definitions(knurl_thread_stacks_control PRIVATE KNURL_STACK_CONTROL=1)
    add_test(NAME knurl.thread-stacks COMMAND knurl_thread_stacks_constrained)
    add_test(NAME knurl.thread-stacks-child COMMAND knurl_thread_stacks_control
      $<TARGET_FILE:knurl_thread_stacks_constrained>)
    set_tests_properties(knurl.thread-stacks knurl.thread-stacks-child PROPERTIES TIMEOUT 10)
  endif()
  add_executable(knurl_target_smoke tests/target-smoke.c)
  target_link_libraries(knurl_target_smoke PRIVATE knurl::shared)
  knurl_c89(knurl_target_smoke)
  knurl_local_runtime(knurl_target_smoke)
  add_test(NAME knurl.target-smoke COMMAND knurl_target_smoke)
  set_tests_properties(knurl.target-smoke PROPERTIES TIMEOUT 10)
  add_test(NAME knurl.sdk-package COMMAND "${CMAKE_COMMAND}"
    "-DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/SDKPackage.cmake")
  set_tests_properties(knurl.sdk-package PROPERTIES TIMEOUT 60)
  add_executable(knurl_vectis_typed tests/vectis-typed.c tests/vectis-forward.c tests/vectis-route.c)
  target_link_libraries(knurl_vectis_typed PRIVATE knurl::shared)
  target_compile_definitions(knurl_vectis_typed PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_vectis_typed)
  knurl_local_runtime(knurl_vectis_typed)
  add_test(NAME knurl.vectis-typed COMMAND knurl_vectis_typed)
  add_executable(knurl_vectis_xml tests/vectis-xml-runtime.c tests/vectis-xml.c tests/vectis-route.c)
  target_link_libraries(knurl_vectis_xml PRIVATE knurl::shared Threads::Threads)
  target_compile_definitions(knurl_vectis_xml PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_vectis_xml)
  knurl_local_runtime(knurl_vectis_xml)
  add_test(NAME knurl.vectis-xml COMMAND knurl_vectis_xml)
  add_executable(knurl_vectis_host tests/vectis-host-runtime.c tests/vectis-host.c tests/vectis-server.c tests/vectis-route.c)
  target_link_libraries(knurl_vectis_host PRIVATE knurl::shared pslog::pslog_shared)
  target_compile_definitions(knurl_vectis_host PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_vectis_host)
  knurl_local_runtime(knurl_vectis_host)
  add_test(NAME knurl.vectis-host COMMAND knurl_vectis_host)
  set_tests_properties(knurl.vectis-host PROPERTIES TIMEOUT 10)
  add_test(NAME knurl.public-boundary COMMAND "${CMAKE_COMMAND}"
    "-DNM=${CMAKE_NM}" "-DLIBRARY=$<TARGET_FILE:knurl_shared>"
    "-DPLATFORM=${CMAKE_SYSTEM_NAME}" -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PublicBoundary.cmake")
  add_executable(knurl_json_runtime tests/json-runtime.c tests/vectis-forward.c tests/vectis-route.c)
  target_link_libraries(knurl_json_runtime PRIVATE knurl::shared)
  target_compile_definitions(knurl_json_runtime PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_json_runtime)
  knurl_local_runtime(knurl_json_runtime)
  add_test(NAME knurl.json-runtime COMMAND knurl_json_runtime)
  add_test(NAME knurl.json-spool-policy COMMAND knurl_json_runtime default-directory)
  set_tests_properties(knurl.json-runtime knurl.json-spool-policy PROPERTIES TIMEOUT 10)
  add_executable(knurl_autoblock tests/autoblock.c)
  target_link_libraries(knurl_autoblock PRIVATE knurl::static Threads::Threads)
  target_compile_definitions(knurl_autoblock PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_autoblock)
  knurl_local_runtime(knurl_autoblock)
  add_test(NAME knurl.autoblock COMMAND knurl_autoblock)
  set_tests_properties(knurl.autoblock PROPERTIES TIMEOUT 5)
  add_executable(knurl_log_backend tests/log-backend.c)
  target_include_directories(knurl_log_backend PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(knurl_log_backend PRIVATE $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  target_link_libraries(knurl_log_backend PRIVATE knurl::static pslog::pslog_static OpenSSL::SSL CURL::libcurl)
  add_dependencies(knurl_log_backend kore-source)
  knurl_c89(knurl_log_backend)
  knurl_local_runtime(knurl_log_backend)
  add_test(NAME knurl.log-backend COMMAND knurl_log_backend)
  add_executable(knurl_input tests/input.c)
  target_include_directories(knurl_input PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(knurl_input PRIVATE knurl::static)
  knurl_c89(knurl_input)
  knurl_local_runtime(knurl_input)
  add_test(NAME knurl.input COMMAND knurl_input)
  set_tests_properties(knurl.input PROPERTIES TIMEOUT 8)
  add_executable(knurl_static_ranges tests/static-ranges.c)
  target_include_directories(knurl_static_ranges PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(knurl_static_ranges PRIVATE knurl::static)
  knurl_c89(knurl_static_ranges)
  knurl_local_runtime(knurl_static_ranges)
  add_test(NAME knurl.static-ranges COMMAND knurl_static_ranges)
  set_tests_properties(knurl.static-ranges PROPERTIES TIMEOUT 8)

  add_executable(knurl_static_encoding tests/static-encoding.c)
  target_include_directories(knurl_static_encoding PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(knurl_static_encoding PRIVATE knurl::static ZLIB::ZLIB)
  knurl_c89(knurl_static_encoding)
  knurl_local_runtime(knurl_static_encoding)
  add_test(NAME knurl.static-encoding COMMAND knurl_static_encoding)
  set_tests_properties(knurl.static-encoding PROPERTIES TIMEOUT 8)
  add_executable(knurl_websocket_protocol tests/websocket-protocol.c)
  target_include_directories(knurl_websocket_protocol PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(knurl_websocket_protocol PRIVATE knurl::static)
  knurl_c89(knurl_websocket_protocol)
  knurl_local_runtime(knurl_websocket_protocol)
  add_test(NAME knurl.websocket-protocol COMMAND knurl_websocket_protocol)
  add_executable(knurl_http_completeness_failure tests/http-completeness-failure.c)
  target_include_directories(knurl_http_completeness_failure PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(knurl_http_completeness_failure PRIVATE knurl::static ZLIB::ZLIB CURL::libcurl)
  knurl_c89(knurl_http_completeness_failure)
  knurl_local_runtime(knurl_http_completeness_failure)
  add_test(NAME knurl.http-completeness-failure COMMAND knurl_http_completeness_failure)
  set_tests_properties(knurl.log-backend PROPERTIES TIMEOUT 5)
  add_executable(knurl_access_log_file tests/access-log-file.c src/access_log_file.c)
  target_compile_definitions(knurl_access_log_file PRIVATE write=knurl_test_write rename=knurl_test_rename open=knurl_test_open
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(knurl_access_log_file PRIVATE knurl::static pslog::pslog_static Threads::Threads)
  knurl_c89(knurl_access_log_file)
  knurl_local_runtime(knurl_access_log_file)
  add_test(NAME knurl.access-log-file COMMAND knurl_access_log_file)
  set_tests_properties(knurl.access-log-file PROPERTIES TIMEOUT 12)
  foreach(codec filesystem-failure vectis-filesystem materialized-output vectis-services-runtime vectis-request-reply vectis-response-head vectis-body-file vectis-route-runtime vectis-config json reply-spooled xml xml-mapped xml-runtime asset mime static-runtime storage-runtime storage-deadline storage-filesystem webdav-runtime source-runtime upload-runtime rest-runtime openapi policy-runtime policy-logs-runtime)
    add_executable(knurl_${codec} "tests/${codec}.c")
    if(codec STREQUAL "xml-runtime")
      target_sources(knurl_${codec} PRIVATE tests/vectis-forward.c tests/vectis-route.c)
    endif()
    if(codec STREQUAL "vectis-config" OR codec STREQUAL "vectis-response-head" OR
        codec STREQUAL "policy-runtime" OR codec STREQUAL "policy-logs-runtime")
      target_sources(knurl_${codec} PRIVATE tests/vectis-server.c)
    endif()
    if(codec STREQUAL "vectis-services-runtime")
      target_sources(knurl_${codec} PRIVATE tests/vectis-services.c)
    endif()
    if(codec STREQUAL "openapi")
      target_sources(knurl_${codec} PRIVATE tests/vectis-services.c tests/vectis-route.c)
    endif()
    if(codec STREQUAL "vectis-route-runtime" OR codec STREQUAL "vectis-body-file" OR codec STREQUAL "vectis-request-reply" OR codec STREQUAL "vectis-services-runtime")
      target_sources(knurl_${codec} PRIVATE tests/vectis-route.c)
    endif()
    if(codec STREQUAL "storage-runtime" OR codec STREQUAL "filesystem-failure" OR codec STREQUAL "storage-deadline")
      # Internal native-job coverage is not an installed SDK consumer.
      target_include_directories(knurl_${codec} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
      target_link_libraries(knurl_${codec} PRIVATE knurl::static Threads::Threads OpenSSL::SSL)
    else()
      target_link_libraries(knurl_${codec} PRIVATE knurl::shared Threads::Threads)
    endif()
    if(codec STREQUAL "policy-runtime" OR codec STREQUAL "policy-logs-runtime" OR codec STREQUAL "vectis-config" OR codec STREQUAL "vectis-response-head")
      target_link_libraries(knurl_${codec} PRIVATE pslog::pslog_shared)
    endif()
    if(codec STREQUAL "static-runtime")
      target_link_libraries(knurl_${codec} PRIVATE ZLIB::ZLIB)
      add_test(NAME knurl.static-runtime-pretty COMMAND knurl_static-runtime pretty)
      set_tests_properties(knurl.static-runtime-pretty PROPERTIES TIMEOUT 8)
    endif()
    target_compile_definitions(knurl_${codec} PRIVATE
      $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
    knurl_c89(knurl_${codec})
    knurl_local_runtime(knurl_${codec})
    add_test(NAME knurl.${codec} COMMAND knurl_${codec})
    set_tests_properties(knurl.${codec} PROPERTIES TIMEOUT 8)
  endforeach()
  add_test(NAME knurl.vectis-response-head-bounded COMMAND knurl_vectis-response-head bounded)
  add_test(NAME knurl.vectis-response-head-plain COMMAND knurl_vectis-response-head plain)
  set_tests_properties(knurl.vectis-response-head-bounded knurl.vectis-response-head-plain
    PROPERTIES TIMEOUT 8)
  add_test(NAME knurl.rest-spill COMMAND knurl_rest-runtime spill)
  set_tests_properties(knurl.rest-spill PROPERTIES TIMEOUT 10)
  add_test(NAME knurl.storage-abandon COMMAND knurl_storage-runtime abandon)
  set_tests_properties(knurl.storage-abandon PROPERTIES TIMEOUT 5)
  add_test(NAME knurl.storage-filesystem-managed COMMAND knurl_storage-filesystem managed)
  set_tests_properties(knurl.storage-filesystem-managed PROPERTIES TIMEOUT 8)
  add_executable(knurl_proxy_websocket tests/proxy-websocket.c)
  target_link_libraries(knurl_proxy_websocket PRIVATE knurl::shared cpkt::openssl_ssl_shared)
  target_compile_definitions(knurl_proxy_websocket PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_proxy_websocket)
  knurl_local_runtime(knurl_proxy_websocket)
  add_test(NAME knurl.proxy-websocket COMMAND knurl_proxy_websocket)
  add_test(NAME knurl.proxy-wss COMMAND knurl_proxy_websocket tls)
  set_tests_properties(knurl.proxy-websocket knurl.proxy-wss PROPERTIES TIMEOUT 10)
  add_executable(knurl_proxy_runtime tests/proxy-runtime.c)
  target_link_libraries(knurl_proxy_runtime PRIVATE knurl::shared)
  target_compile_definitions(knurl_proxy_runtime PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_proxy_runtime)
  knurl_local_runtime(knurl_proxy_runtime)
  add_test(NAME knurl.proxy-runtime COMMAND knurl_proxy_runtime)
  set_tests_properties(knurl.proxy-runtime PROPERTIES TIMEOUT 10)
  add_executable(knurl_proxy_h2 tests/proxy-h2.c)
  target_link_libraries(knurl_proxy_h2 PRIVATE knurl::shared
    cpkt::openssl_ssl_shared cpkt::nghttp2_shared)
  target_compile_definitions(knurl_proxy_h2 PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_proxy_h2)
  knurl_local_runtime(knurl_proxy_h2)
  add_test(NAME knurl.proxy-h2 COMMAND knurl_proxy_h2)
  set_tests_properties(knurl.proxy-h2 PROPERTIES TIMEOUT 8)
  foreach(surface framing headers upload http http-upstream http-wire url director response local ws-handshake ws-wire ws-rejection transport)
    string(REPLACE "-" "_" target "knurl_proxy_${surface}")
    add_executable(${target} "tests/proxy-${surface}.c")
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
    target_compile_definitions(${target} PRIVATE _POSIX_C_SOURCE=200809L
      $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
    target_link_libraries(${target} PRIVATE knurl::static CURL::libcurl OpenSSL::SSL Threads::Threads)
    knurl_c89(${target})
    knurl_local_runtime(${target})
    add_test(NAME "knurl.proxy-${surface}" COMMAND ${target})
    set_tests_properties("knurl.proxy-${surface}" PROPERTIES TIMEOUT 10)
  endforeach()
  add_executable(knurl_websocket_runtime tests/websocket-runtime.c)
  target_compile_definitions(knurl_websocket_runtime PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(knurl_websocket_runtime PRIVATE knurl::shared
    cpkt::openssl_ssl_shared Threads::Threads)
  knurl_c89(knurl_websocket_runtime)
  knurl_local_runtime(knurl_websocket_runtime)
  add_test(NAME knurl.websocket-runtime COMMAND knurl_websocket_runtime)
  add_test(NAME knurl.websocket-tls COMMAND knurl_websocket_runtime tls)
  set_tests_properties(knurl.websocket-runtime knurl.websocket-tls PROPERTIES TIMEOUT 15)

  add_executable(knurl_acme_protocol tests/acme-protocol.c tests/acme-ca.c tests/vectis-services.c tests/vectis-route.c)
  target_compile_definitions(knurl_acme_protocol PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_link_libraries(knurl_acme_protocol PRIVATE knurl::shared
    cpkt::openssl_ssl_shared lonejson::lonejson Threads::Threads)
  knurl_c89(knurl_acme_protocol)
  knurl_local_runtime(knurl_acme_protocol)
  add_test(NAME knurl.acme-protocol COMMAND knurl_acme_protocol)
  add_test(NAME knurl.acme-trust-environment COMMAND knurl_acme_protocol environment)
  add_test(NAME knurl.acme-trust-directory COMMAND knurl_acme_protocol directory)
  add_test(NAME knurl.acme-receive-limit COMMAND knurl_acme_protocol limit)
  add_test(NAME knurl.acme-transport-timeout COMMAND knurl_acme_protocol timeout)
  add_test(NAME knurl.acme-account-errors COMMAND knurl_acme_protocol badnonce)
  set_tests_properties(knurl.acme-protocol knurl.acme-trust-environment
    knurl.acme-trust-directory knurl.acme-receive-limit
    knurl.acme-transport-timeout knurl.acme-account-errors PROPERTIES TIMEOUT 10)
  add_executable(knurl_acme_runtime tests/acme-runtime.c tests/acme-backend.c)
  add_dependencies(knurl_acme_runtime kore-source)
  target_include_directories(knurl_acme_runtime PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(knurl_acme_runtime PRIVATE
    $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  knurl_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/acme-backend.c")
  target_link_libraries(knurl_acme_runtime PRIVATE knurl::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  knurl_c89(knurl_acme_runtime)
  knurl_local_runtime(knurl_acme_runtime)
  add_test(NAME knurl.acme-runtime COMMAND knurl_acme_runtime)
  add_test(NAME knurl.acme-persist-failure COMMAND knurl_acme_runtime fail)
  add_test(NAME knurl.acme-persist-timeout COMMAND knurl_acme_runtime timeout)
  add_test(NAME knurl.acme-pending COMMAND knurl_acme_runtime pending)
  add_test(NAME knurl.tls-unbound COMMAND knurl_acme_runtime unbound)
  set_tests_properties(knurl.acme-runtime knurl.acme-persist-failure
    knurl.acme-persist-timeout knurl.acme-pending knurl.tls-unbound PROPERTIES TIMEOUT 8)
  add_executable(knurl_acme_state tests/acme-state.c)
  target_compile_definitions(knurl_acme_state PRIVATE
    $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  target_include_directories(knurl_acme_state PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_link_libraries(knurl_acme_state PRIVATE knurl::static Threads::Threads OpenSSL::SSL)
  knurl_c89(knurl_acme_state)
  knurl_local_runtime(knurl_acme_state)
  add_test(NAME knurl.acme-state COMMAND knurl_acme_state)
  set_tests_properties(knurl.acme-state PROPERTIES TIMEOUT 12)
  add_executable(knurl_auxiliary_runtime tests/auxiliary-runtime.c)
  add_dependencies(knurl_auxiliary_runtime kore-source)
  target_include_directories(knurl_auxiliary_runtime PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(knurl_auxiliary_runtime PRIVATE
    $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  knurl_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/auxiliary-runtime.c")
  target_link_libraries(knurl_auxiliary_runtime PRIVATE knurl::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  knurl_c89(knurl_auxiliary_runtime)
  knurl_local_runtime(knurl_auxiliary_runtime)
  add_test(NAME knurl.auxiliary-runtime COMMAND knurl_auxiliary_runtime)
  add_test(NAME knurl.auxiliary-restart COMMAND knurl_auxiliary_runtime restart)
  add_test(NAME knurl.auxiliary-terminate COMMAND knurl_auxiliary_runtime terminate)
  set_tests_properties(knurl.auxiliary-runtime knurl.auxiliary-restart knurl.auxiliary-terminate PROPERTIES TIMEOUT 8)
  add_executable(knurl_process_runtime tests/process-runtime.c)
  target_link_libraries(knurl_process_runtime PRIVATE knurl::shared Threads::Threads)
  knurl_c89(knurl_process_runtime)
  knurl_local_runtime(knurl_process_runtime)
  add_test(NAME knurl.process-runtime COMMAND knurl_process_runtime)
  set_tests_properties(knurl.process-runtime PROPERTIES TIMEOUT 8)
  add_executable(knurl_process_backend tests/process-runtime.c tests/process-backend.c)
  add_dependencies(knurl_process_backend kore-source)
  target_include_directories(knurl_process_backend PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(knurl_process_backend PRIVATE KNURL_PROCESS_BACKEND_TEST=1
    $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  knurl_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/process-backend.c")
  target_link_libraries(knurl_process_backend PRIVATE knurl::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  knurl_c89(knurl_process_backend)
  knurl_local_runtime(knurl_process_backend)
  add_test(NAME knurl.process-backend COMMAND knurl_process_backend)
  set_tests_properties(knurl.process-backend PROPERTIES TIMEOUT 8)
  add_executable(knurl_tls_runtime tests/tls-runtime.c tests/vectis-services.c tests/vectis-route.c)
  target_link_libraries(knurl_tls_runtime PRIVATE knurl::shared cpkt::openssl_ssl_shared Threads::Threads)
  target_compile_definitions(knurl_tls_runtime PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
  knurl_c89(knurl_tls_runtime)
  knurl_local_runtime(knurl_tls_runtime)
  add_test(NAME knurl.tls-runtime COMMAND knurl_tls_runtime)
  set_tests_properties(knurl.tls-runtime PROPERTIES TIMEOUT 15)
  add_executable(knurl_channel_runtime tests/channel-runtime.c)
  target_link_libraries(knurl_channel_runtime PRIVATE knurl::shared Threads::Threads)
  knurl_c89(knurl_channel_runtime)
  knurl_local_runtime(knurl_channel_runtime)
  add_test(NAME knurl.channel-runtime COMMAND knurl_channel_runtime)
  set_tests_properties(knurl.channel-runtime PROPERTIES TIMEOUT 12)
  add_executable(knurl_channel_backend tests/channel-runtime.c tests/channel-backend.c)
  add_dependencies(knurl_channel_backend kore-source)
  target_include_directories(knurl_channel_backend PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(knurl_channel_backend PRIVATE KNURL_CHANNEL_BACKEND_TEST=1
    $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  knurl_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/channel-backend.c")
  target_link_libraries(knurl_channel_backend PRIVATE knurl::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  knurl_c89(knurl_channel_backend)
  knurl_local_runtime(knurl_channel_backend)
  add_test(NAME knurl.channel-backend COMMAND knurl_channel_backend)
  set_tests_properties(knurl.channel-backend PROPERTIES TIMEOUT 12)
  add_executable(knurl_body_runtime tests/body-runtime.c)
  target_link_libraries(knurl_body_runtime PRIVATE knurl::shared Threads::Threads)
  knurl_c89(knurl_body_runtime)
  knurl_local_runtime(knurl_body_runtime)
  add_test(NAME knurl.body-runtime COMMAND knurl_body_runtime)
  add_test(NAME knurl.body-rate COMMAND knurl_body_runtime rate)
  add_test(NAME knurl.body-unlimited COMMAND knurl_body_runtime unlimited)
  set_tests_properties(knurl.body-runtime knurl.body-rate knurl.body-unlimited
    PROPERTIES TIMEOUT 12)
  add_executable(knurl_response_timeout tests/response-timeout.c)
  add_dependencies(knurl_response_timeout kore-source)
  target_include_directories(knurl_response_timeout PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(knurl_response_timeout PRIVATE $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  knurl_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/response-timeout.c")
  target_link_libraries(knurl_response_timeout PRIVATE knurl::static Threads::Threads
    OpenSSL::SSL CURL::libcurl lonejson::lonejson_static pslog::pslog_static)
  knurl_c89(knurl_response_timeout)
  knurl_local_runtime(knurl_response_timeout)
  add_test(NAME knurl.response-timeout COMMAND knurl_response_timeout)
  set_tests_properties(knurl.response-timeout PROPERTIES TIMEOUT 5)
  add_executable(knurl_response_runtime tests/response-runtime.c)
  target_link_libraries(knurl_response_runtime PRIVATE knurl::shared)
  knurl_c89(knurl_response_runtime)
  knurl_local_runtime(knurl_response_runtime)
  add_test(NAME knurl.response-runtime COMMAND knurl_response_runtime)
  add_test(NAME knurl.response-minimum-queue COMMAND knurl_response_runtime minimum)
  set_tests_properties(knurl.response-runtime knurl.response-minimum-queue PROPERTIES TIMEOUT 15)
  add_executable(knurl_loop_smoke tests/loop-smoke.c)
  target_link_libraries(knurl_loop_smoke PRIVATE knurl::shared)
  knurl_c89(knurl_loop_smoke)
  knurl_local_runtime(knurl_loop_smoke)
  add_test(NAME knurl.loop-smoke COMMAND knurl_loop_smoke)
  set_tests_properties(knurl.loop-smoke PROPERTIES TIMEOUT 10)
  add_executable(knurl_server_runtime tests/server-runtime.c)
  target_link_libraries(knurl_server_runtime PRIVATE knurl::shared Threads::Threads)
  knurl_c89(knurl_server_runtime)
  knurl_local_runtime(knurl_server_runtime)
  add_test(NAME knurl.server-runtime COMMAND knurl_server_runtime)
  set_tests_properties(knurl.server-runtime PROPERTIES TIMEOUT 30)
  foreach(variant static shared)
    add_executable(knurl_server_preflight_${variant} tests/server-preflight.c)
    target_link_libraries(knurl_server_preflight_${variant} PRIVATE knurl::${variant})
    target_compile_definitions(knurl_server_preflight_${variant} PRIVATE $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
    knurl_c89(knurl_server_preflight_${variant})
    knurl_local_runtime(knurl_server_preflight_${variant})
    add_test(NAME knurl.server-preflight.${variant} COMMAND knurl_server_preflight_${variant})
    set_tests_properties(knurl.server-preflight.${variant} PROPERTIES TIMEOUT 10)
  endforeach()
  add_executable(knurl_kore_runtime tests/kore-runtime.c $<TARGET_OBJECTS:knurl_kore>)
  add_dependencies(knurl_kore_runtime kore-source)
  knurl_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/kore-runtime.c" "${CMAKE_CURRENT_SOURCE_DIR}/tests/kore-port.c")
  target_compile_definitions(knurl_kore_runtime PRIVATE $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  target_include_directories(knurl_kore_runtime PRIVATE "${kore_source_dir}/include/kore")
  target_link_libraries(knurl_kore_runtime PRIVATE cpkt::openssl_ssl_shared cpkt::curl_shared
    lonejson::lonejson pslog::pslog_shared)
  knurl_c89(knurl_kore_runtime)
  knurl_local_runtime(knurl_kore_runtime)
  add_test(NAME knurl.kore-runtime COMMAND knurl_kore_runtime)
  set_tests_properties(knurl.kore-runtime PROPERTIES TIMEOUT 20)
  add_executable(knurl_kore_json_stack tests/kore-json-stack.c)
  add_dependencies(knurl_kore_json_stack kore-source)
  knurl_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/kore-json-stack.c")
  target_compile_definitions(knurl_kore_json_stack PRIVATE
    $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  target_include_directories(knurl_kore_json_stack PRIVATE
    "${kore_source_dir}/include/kore" "${kore_source_dir}/src")
  target_link_libraries(knurl_kore_json_stack PRIVATE knurl::static
    pslog::pslog_shared OpenSSL::SSL CURL::libcurl)
  knurl_c89(knurl_kore_json_stack)
  knurl_local_runtime(knurl_kore_json_stack)
  add_test(NAME knurl.kore-json-stack COMMAND knurl_kore_json_stack)
  set_tests_properties(knurl.kore-json-stack PROPERTIES TIMEOUT 15)
  add_executable(knurl_xml_stack tests/xml-stack.c)
  target_include_directories(knurl_xml_stack PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
  target_compile_definitions(knurl_xml_stack PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64)
  target_link_libraries(knurl_xml_stack PRIVATE knurl::static LibXml2::LibXml2)
  knurl_c89(knurl_xml_stack)
  knurl_local_runtime(knurl_xml_stack)
  add_test(NAME knurl.xml-stack COMMAND knurl_xml_stack)
  set_tests_properties(knurl.xml-stack PROPERTIES TIMEOUT 15)
  if(KNURL_TARGET MATCHES "-musl$")
    target_compile_definitions(knurl_xml_stack PRIVATE KNURL_MUSL_STACK_CHECK=1)
    target_compile_definitions(knurl_kore_json_stack PRIVATE KNURL_MUSL_STACK_CHECK=1)
  endif()
  add_executable(knurl_traversal_benchmark tests/traversal-benchmark.c)
  add_dependencies(knurl_traversal_benchmark kore-source)
  target_include_directories(knurl_traversal_benchmark PRIVATE "${kore_source_dir}/include/kore")
  target_compile_definitions(knurl_traversal_benchmark PRIVATE
    $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  target_link_libraries(knurl_traversal_benchmark PRIVATE knurl::static
    pslog::pslog_shared OpenSSL::SSL CURL::libcurl)
  knurl_c89(knurl_traversal_benchmark)
  knurl_local_runtime(knurl_traversal_benchmark)
  add_test(NAME knurl.traversal-performance-contract COMMAND "${CMAKE_COMMAND}"
    "-DBENCHMARK=$<TARGET_FILE:knurl_traversal_benchmark>"
    "-DWORK=${CMAKE_CURRENT_BINARY_DIR}/tests/traversal-performance-contract"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/TraversalPerformanceContract.cmake")
  set_tests_properties(knurl.traversal-performance-contract PROPERTIES TIMEOUT 5)
  add_executable(knurl_kore_request_order tests/kore-request-order.c $<TARGET_OBJECTS:knurl_kore>)
  add_dependencies(knurl_kore_request_order kore-source)
  knurl_kore_object_dependencies("${kore_source_dir}/.patched"
    "${CMAKE_CURRENT_SOURCE_DIR}/tests/kore-request-order.c")
  target_compile_definitions(knurl_kore_request_order PRIVATE $<TARGET_PROPERTY:knurl_kore,COMPILE_DEFINITIONS>)
  target_include_directories(knurl_kore_request_order PRIVATE "${kore_source_dir}/include/kore")
  target_link_libraries(knurl_kore_request_order PRIVATE cpkt::openssl_ssl_shared cpkt::curl_shared
    lonejson::lonejson pslog::pslog_shared)
  knurl_c89(knurl_kore_request_order)
  knurl_local_runtime(knurl_kore_request_order)
  add_test(NAME knurl.kore-request-order COMMAND knurl_kore_request_order)
  set_tests_properties(knurl.kore-request-order PROPERTIES TIMEOUT 5)
  add_test(NAME kore.backend-invalidation COMMAND "${CMAKE_COMMAND}"
    "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/backend invalidation"
    "-DTOOLCHAIN=${knurl_toolchain}" "-DKNURL_TARGET=${KNURL_TARGET}"
    "-DKNURL_CAN_RUN=${knurl_can_run}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BackendInvalidation.cmake")
  set_tests_properties(kore.backend-invalidation PROPERTIES TIMEOUT 60)
  add_executable(knurl_declarations tests/declarations.c)
  target_link_libraries(knurl_declarations PRIVATE knurl::shared)
  knurl_c89(knurl_declarations)
  knurl_local_runtime(knurl_declarations)
  add_test(NAME knurl.declarations COMMAND knurl_declarations)
  add_test(NAME lifecycle.version COMMAND "${CMAKE_COMMAND}"
    "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/version"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Version.cmake")
  add_test(NAME lifecycle.failures COMMAND "${CMAKE_COMMAND}"
    "-DKNURL_BUILD=${CMAKE_CURRENT_BINARY_DIR}"
    "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/lifecycle"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Lifecycle.cmake")
  set_tests_properties(lifecycle.version lifecycle.failures PROPERTIES TIMEOUT 15)
  foreach(contract OwnedPaths SourceInputs Testing KoreOwnership Privacy)
    add_test(NAME lifecycle.${contract} COMMAND "${CMAKE_COMMAND}"
      "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
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
  add_executable(knurl_reply_test tests/reply.c)
  target_link_libraries(knurl_reply_test PRIVATE knurl::shared)
  knurl_c89(knurl_reply_test)
  knurl_local_runtime(knurl_reply_test)
  add_test(NAME knurl.reply COMMAND knurl_reply_test)
  add_executable(knurl_source_file_test tests/source-file.c)
  target_link_libraries(knurl_source_file_test PRIVATE knurl::shared)
  knurl_c89(knurl_source_file_test)
  knurl_local_runtime(knurl_source_file_test)
  add_test(NAME knurl.source-file COMMAND knurl_source_file_test)
  set_tests_properties(knurl.source-file PROPERTIES TIMEOUT 5)
  add_executable(knurl_kore_port tests/kore-port.c "${kore_source_dir}/src/sha2.c")
  add_dependencies(knurl_kore_port kore-source)
  target_compile_definitions(knurl_kore_port PRIVATE _GNU_SOURCE=1 TLS_BACKEND_OPENSSL)
  target_include_directories(knurl_kore_port PRIVATE "${kore_source_dir}/include/kore")
  target_link_libraries(knurl_kore_port PRIVATE OpenSSL::SSL)
  knurl_c89(knurl_kore_port)
  knurl_local_runtime(knurl_kore_port)
  add_test(NAME knurl.kore-port COMMAND knurl_kore_port)
  add_executable(knurl_foundation tests/foundation.c)
  target_link_libraries(knurl_foundation PRIVATE knurl::shared)
  knurl_c89(knurl_foundation)
  knurl_local_runtime(knurl_foundation)
  add_test(NAME knurl.foundation COMMAND knurl_foundation)
  add_executable(knurl_headers_cpp tests/headers.cpp)
  target_link_libraries(knurl_headers_cpp PRIVATE knurl::shared pslog::pslog_shared)
  set_target_properties(knurl_headers_cpp PROPERTIES CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  target_compile_options(knurl_headers_cpp PRIVATE -Wall -Wextra -Wpedantic -Werror)
  knurl_local_runtime(knurl_headers_cpp)
  add_test(NAME knurl.headers.cpp COMMAND knurl_headers_cpp)
  find_package(Git REQUIRED)
  add_test(NAME knurl.installed-consumer COMMAND "${CMAKE_COMMAND}"
    "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DKNURL_BINARY=${CMAKE_CURRENT_BINARY_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/consumer"
    "-DKNURL_SDK_ROOT=${KNURL_SDK_ROOT}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/InstalledConsumer.cmake")
  set_tests_properties(knurl.installed-consumer PROPERTIES TIMEOUT 60 FIXTURES_SETUP installed-sdk)
  foreach(variant static shared)
    add_test(NAME "knurl.installed-consumer.build-${variant}" COMMAND "${CMAKE_COMMAND}"
      "-DKNURL_CANDIDATE_VERSION=${KNURL_CANDIDATE_VERSION}"
      "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/consumer" "-DVARIANT=${variant}"
      "-DTOOLCHAIN=${knurl_toolchain}" "-DKNURL_TARGET=${KNURL_TARGET}"
      "-DKNURL_TEST_BUILD_TYPE=${CMAKE_BUILD_TYPE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildConsumer.cmake")
    set_tests_properties("knurl.installed-consumer.build-${variant}" PROPERTIES TIMEOUT 60
      FIXTURES_REQUIRED installed-sdk FIXTURES_SETUP "installed-${variant}")
    add_test(NAME "knurl.installed-consumer.${variant}" COMMAND "${CMAKE_COMMAND}"
      "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/consumer" "-DVARIANT=${variant}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/RunConsumers.cmake")
    set_tests_properties("knurl.installed-consumer.${variant}" PROPERTIES TIMEOUT 60
      FIXTURES_REQUIRED "installed-${variant}")
  endforeach()
  add_test(NAME kore.patch-replay
    COMMAND "${CMAKE_COMMAND}"
      "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/patch-replay"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PatchReplay.cmake")
  add_test(NAME kore.preparation
    COMMAND "${CMAKE_COMMAND}"
      "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/preparation"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Preparation.cmake")
  add_test(NAME kore.build-invalidation
    COMMAND "${CMAKE_COMMAND}"
      "-DKNURL_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/build invalidation"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildInvalidation.cmake")
  find_program(BASH_EXECUTABLE bash REQUIRED)
  add_test(NAME kore.upgrade
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/upgrade-kore.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tests/upgrade path")
  set_tests_properties(kore.patch-replay kore.preparation kore.build-invalidation
    kore.upgrade PROPERTIES TIMEOUT 60)
  if(NOT knurl_can_run)
    # knurl.* checks execute selected-target programs, except these host-tool
    # gates. Keep new runtime cases from silently escaping cross-target gating.
    get_property(runtime_tests DIRECTORY PROPERTY TESTS)
    list(FILTER runtime_tests INCLUDE REGEX "^knurl\\.")
    list(REMOVE_ITEM runtime_tests knurl.public-boundary knurl.installed-consumer
      knurl.sdk-package)
    set_tests_properties(${runtime_tests} PROPERTIES DISABLED TRUE)
  endif()
endif()
