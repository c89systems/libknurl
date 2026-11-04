cmake_minimum_required(VERSION 3.24)
include("${ROTMOS_ROOT}/cmake/OwnedPaths.cmake")
rotmos_require_owned_path("${TEST_ROOT}" "Consumer fixture")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/source")
file(COPY "${ROTMOS_ROOT}/tests/consumer/CMakeLists.txt"
  "${ROTMOS_ROOT}/tests/foundation.c" "${ROTMOS_ROOT}/tests/reply.c"
  "${ROTMOS_ROOT}/tests/declarations.c"
  "${ROTMOS_ROOT}/tests/server-runtime.c"
  "${ROTMOS_ROOT}/tests/proxy-runtime.c"
  "${ROTMOS_ROOT}/tests/proxy-websocket.c"
  "${ROTMOS_ROOT}/tests/json.c" "${ROTMOS_ROOT}/tests/xml.c" "${ROTMOS_ROOT}/tests/xml-mapped.c" "${ROTMOS_ROOT}/tests/xml-runtime.c"
  "${ROTMOS_ROOT}/tests/json-runtime.c"
  "${ROTMOS_ROOT}/tests/vectis-forward.c" "${ROTMOS_ROOT}/tests/vectis-forward.h"
  "${ROTMOS_ROOT}/tests/vectis-config.c" "${ROTMOS_ROOT}/tests/vectis-server.c" "${ROTMOS_ROOT}/tests/vectis-server.h"
  "${ROTMOS_ROOT}/tests/vectis-route-runtime.c" "${ROTMOS_ROOT}/tests/vectis-route.c" "${ROTMOS_ROOT}/tests/vectis-route.h"
  "${ROTMOS_ROOT}/tests/vectis-body-file.c"
  "${ROTMOS_ROOT}/tests/vectis-response-head.c"
  "${ROTMOS_ROOT}/tests/vectis-request-reply.c"
  "${ROTMOS_ROOT}/tests/vectis-services.c" "${ROTMOS_ROOT}/tests/vectis-services.h" "${ROTMOS_ROOT}/tests/vectis-services-runtime.c"
  "${ROTMOS_ROOT}/tests/asset.c" "${ROTMOS_ROOT}/tests/static-runtime.c"
  "${ROTMOS_ROOT}/tests/storage-filesystem.c" "${ROTMOS_ROOT}/tests/webdav-runtime.c"
  "${ROTMOS_ROOT}/tests/source-runtime.c" "${ROTMOS_ROOT}/tests/upload-runtime.c" "${ROTMOS_ROOT}/tests/rest-runtime.c" "${ROTMOS_ROOT}/tests/openapi.c" "${ROTMOS_ROOT}/tests/policy-runtime.c"
  "${ROTMOS_ROOT}/tests/policy-logs-runtime.c"
  DESTINATION "${TEST_ROOT}/source")
file(STRINGS "${ROTMOS_ROOT}/VERSION" version LIMIT_COUNT 1)
function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE diagnostic)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer command failed: ${ARGV}\n${log}${diagnostic}")
  endif()
endfunction()
run("${CMAKE_COMMAND}" --install "${ROTMOS_BINARY}" --prefix "${TEST_ROOT}/installed")
foreach(private_header executor.h framing.h)
  if(EXISTS "${TEST_ROOT}/installed/include/rotmos/${private_header}")
    message(FATAL_ERROR "Private machinery leaked into the SDK: ${private_header}")
  endif()
endforeach()
foreach(provider cpkt lonejson pslog)
  file(COPY "${ROTMOS_SDK_ROOT}/${provider}" DESTINATION "${TEST_ROOT}/installed/providers")
endforeach()
file(RENAME "${TEST_ROOT}/installed" "${TEST_ROOT}/relocated SDK")
foreach(variant static shared)
  run("${CMAKE_COMMAND}" -S "${TEST_ROOT}/source" -B "${TEST_ROOT}/${variant}" -G Ninja
    "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}" "-DROTMOS_TARGET=${ROTMOS_TARGET}"
    "-DROTMOS_PREFIX=${TEST_ROOT}/relocated SDK" "-DROTMOS_VARIANT=${variant}"
    "-DROTMOS_VERSION=${version}")
  run("${CMAKE_COMMAND}" --build "${TEST_ROOT}/${variant}" --parallel 1)
  if(ROTMOS_CAN_RUN)
    run("${TEST_ROOT}/${variant}/foundation_consumer")
    run("${TEST_ROOT}/${variant}/reply_consumer")
    run("${TEST_ROOT}/${variant}/declarations_consumer")
    run("${TEST_ROOT}/${variant}/vectis-config_consumer")
    run("${TEST_ROOT}/${variant}/vectis-route-runtime_consumer")
    run("${TEST_ROOT}/${variant}/vectis-body-file_consumer")
    run("${TEST_ROOT}/${variant}/vectis-request-reply_consumer")
    run("${TEST_ROOT}/${variant}/vectis-services-runtime_consumer")
    run("${TEST_ROOT}/${variant}/vectis-response-head_consumer")
    run("${TEST_ROOT}/${variant}/vectis-response-head_consumer" bounded)
    run("${TEST_ROOT}/${variant}/vectis-response-head_consumer" plain)
    run("${TEST_ROOT}/${variant}/server-runtime_consumer")
    run("${TEST_ROOT}/${variant}/proxy-runtime_consumer")
    run("${TEST_ROOT}/${variant}/proxy-websocket_consumer")
    run("${TEST_ROOT}/${variant}/proxy-websocket_consumer" tls)
    run("${TEST_ROOT}/${variant}/json_consumer")
    run("${TEST_ROOT}/${variant}/xml_consumer")
    run("${TEST_ROOT}/${variant}/xml-mapped_consumer")
    run("${TEST_ROOT}/${variant}/xml-runtime_consumer")
    run("${TEST_ROOT}/${variant}/asset_consumer")
    run("${TEST_ROOT}/${variant}/static-runtime_consumer")
    run("${TEST_ROOT}/${variant}/storage-filesystem_consumer")
    run("${TEST_ROOT}/${variant}/storage-filesystem_consumer" managed)
    run("${TEST_ROOT}/${variant}/webdav-runtime_consumer")
    run("${TEST_ROOT}/${variant}/source-runtime_consumer")
    run("${TEST_ROOT}/${variant}/upload-runtime_consumer")
    run("${TEST_ROOT}/${variant}/rest-runtime_consumer")
    run("${TEST_ROOT}/${variant}/openapi_consumer")
    run("${TEST_ROOT}/${variant}/policy-runtime_consumer")
    run("${TEST_ROOT}/${variant}/policy-logs-runtime_consumer")
    run("${TEST_ROOT}/${variant}/json-runtime_consumer")
    run("${TEST_ROOT}/${variant}/json-runtime_consumer" default-directory)
  endif()
endforeach()
