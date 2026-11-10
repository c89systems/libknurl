cmake_minimum_required(VERSION 3.24)
include("${KNURL_ROOT}/cmake/OwnedPaths.cmake")
knurl_require_owned_path("${TEST_ROOT}" "Consumer fixture")
file(MAKE_DIRECTORY "${TEST_ROOT}/source")
file(WRITE "${TEST_ROOT}/prepare.log" "")
file(COPY "${KNURL_ROOT}/tests/consumer/CMakeLists.txt"
  "${KNURL_ROOT}/tests/foundation.c" "${KNURL_ROOT}/tests/reply.c"
  "${KNURL_ROOT}/tests/declarations.c"
  "${KNURL_ROOT}/tests/server-runtime.c"
  "${KNURL_ROOT}/tests/proxy-runtime.c"
  "${KNURL_ROOT}/tests/proxy-websocket.c"
  "${KNURL_ROOT}/tests/json.c" "${KNURL_ROOT}/tests/xml.c" "${KNURL_ROOT}/tests/xml-mapped.c" "${KNURL_ROOT}/tests/xml-runtime.c"
  "${KNURL_ROOT}/tests/json-runtime.c"
  "${KNURL_ROOT}/tests/vectis-forward.c" "${KNURL_ROOT}/tests/vectis-forward.h"
  "${KNURL_ROOT}/tests/vectis-typed.c"
  "${KNURL_ROOT}/tests/vectis-filesystem.c" "${KNURL_ROOT}/tests/vectis-filesystem.h"
  "${KNURL_ROOT}/tests/vectis-xml-runtime.c" "${KNURL_ROOT}/tests/vectis-xml.c"
  "${KNURL_ROOT}/tests/vectis-xml.h"
  "${KNURL_ROOT}/tests/vectis-host-runtime.c" "${KNURL_ROOT}/tests/vectis-host.c"
  "${KNURL_ROOT}/tests/vectis-host.h"
  "${KNURL_ROOT}/tests/vectis-config.c" "${KNURL_ROOT}/tests/vectis-server.c" "${KNURL_ROOT}/tests/vectis-server.h"
  "${KNURL_ROOT}/tests/vectis-route-runtime.c" "${KNURL_ROOT}/tests/vectis-route.c" "${KNURL_ROOT}/tests/vectis-route.h"
  "${KNURL_ROOT}/tests/vectis-body-file.c"
  "${KNURL_ROOT}/tests/vectis-response-head.c"
  "${KNURL_ROOT}/tests/vectis-request-reply.c"
  "${KNURL_ROOT}/tests/tls-runtime.c"
  "${KNURL_ROOT}/tests/materialized-output.c"
  "${KNURL_ROOT}/tests/vectis-services.c" "${KNURL_ROOT}/tests/vectis-services.h" "${KNURL_ROOT}/tests/vectis-services-runtime.c"
  "${KNURL_ROOT}/tests/asset.c" "${KNURL_ROOT}/tests/mime.c" "${KNURL_ROOT}/tests/static-runtime.c"
  "${KNURL_ROOT}/tests/storage-filesystem.c" "${KNURL_ROOT}/tests/webdav-runtime.c"
  "${KNURL_ROOT}/tests/source-runtime.c" "${KNURL_ROOT}/tests/upload-runtime.c" "${KNURL_ROOT}/tests/rest-runtime.c" "${KNURL_ROOT}/tests/openapi.c" "${KNURL_ROOT}/tests/policy-runtime.c"
  "${KNURL_ROOT}/tests/policy-logs-runtime.c"
  DESTINATION "${TEST_ROOT}/source")
function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE diagnostic)
  file(APPEND "${TEST_ROOT}/prepare.log" "${ARGV}\n${log}${diagnostic}\n")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer command failed: ${ARGV}\n${log}${diagnostic}")
  endif()
endfunction()
run("${CMAKE_COMMAND}" --install "${KNURL_BINARY}" --prefix "${TEST_ROOT}/installed")
foreach(private_header executor.h framing.h)
  if(EXISTS "${TEST_ROOT}/installed/include/knurl/${private_header}")
    message(FATAL_ERROR "Private machinery leaked into the SDK: ${private_header}")
  endif()
endforeach()
foreach(provider cpkt lonejson pslog)
  file(COPY "${KNURL_SDK_ROOT}/${provider}" DESTINATION "${TEST_ROOT}/installed/providers")
endforeach()
file(COPY "${TEST_ROOT}/installed/" DESTINATION "${TEST_ROOT}/relocated SDK")
# Retain consumer objects and unchanged SDK timestamps, while making the original
# install location unavailable throughout configure/link/execution.
file(REMOVE_RECURSE "${TEST_ROOT}/unavailable")
file(RENAME "${TEST_ROOT}/installed" "${TEST_ROOT}/unavailable")
