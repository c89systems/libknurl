cmake_minimum_required(VERSION 3.24)
include("${ROTMOS_ROOT}/cmake/OwnedPaths.cmake")
rotmos_require_owned_path("${TEST_ROOT}" "Consumer fixture")
file(MAKE_DIRECTORY "${TEST_ROOT}/source")
file(WRITE "${TEST_ROOT}/prepare.log" "")
file(COPY "${ROTMOS_ROOT}/tests/consumer/CMakeLists.txt"
  "${ROTMOS_ROOT}/tests/foundation.c" "${ROTMOS_ROOT}/tests/reply.c"
  "${ROTMOS_ROOT}/tests/declarations.c"
  "${ROTMOS_ROOT}/tests/server-runtime.c"
  "${ROTMOS_ROOT}/tests/proxy-runtime.c"
  "${ROTMOS_ROOT}/tests/proxy-websocket.c"
  "${ROTMOS_ROOT}/tests/json.c" "${ROTMOS_ROOT}/tests/xml.c" "${ROTMOS_ROOT}/tests/xml-mapped.c" "${ROTMOS_ROOT}/tests/xml-runtime.c"
  "${ROTMOS_ROOT}/tests/json-runtime.c"
  "${ROTMOS_ROOT}/tests/vectis-forward.c" "${ROTMOS_ROOT}/tests/vectis-forward.h"
  "${ROTMOS_ROOT}/tests/vectis-typed.c"
  "${ROTMOS_ROOT}/tests/vectis-filesystem.c" "${ROTMOS_ROOT}/tests/vectis-filesystem.h"
  "${ROTMOS_ROOT}/tests/vectis-xml-runtime.c" "${ROTMOS_ROOT}/tests/vectis-xml.c"
  "${ROTMOS_ROOT}/tests/vectis-xml.h"
  "${ROTMOS_ROOT}/tests/vectis-host-runtime.c" "${ROTMOS_ROOT}/tests/vectis-host.c"
  "${ROTMOS_ROOT}/tests/vectis-host.h"
  "${ROTMOS_ROOT}/tests/vectis-config.c" "${ROTMOS_ROOT}/tests/vectis-server.c" "${ROTMOS_ROOT}/tests/vectis-server.h"
  "${ROTMOS_ROOT}/tests/vectis-route-runtime.c" "${ROTMOS_ROOT}/tests/vectis-route.c" "${ROTMOS_ROOT}/tests/vectis-route.h"
  "${ROTMOS_ROOT}/tests/vectis-body-file.c"
  "${ROTMOS_ROOT}/tests/vectis-response-head.c"
  "${ROTMOS_ROOT}/tests/vectis-request-reply.c"
  "${ROTMOS_ROOT}/tests/tls-runtime.c"
  "${ROTMOS_ROOT}/tests/materialized-output.c"
  "${ROTMOS_ROOT}/tests/vectis-services.c" "${ROTMOS_ROOT}/tests/vectis-services.h" "${ROTMOS_ROOT}/tests/vectis-services-runtime.c"
  "${ROTMOS_ROOT}/tests/asset.c" "${ROTMOS_ROOT}/tests/mime.c" "${ROTMOS_ROOT}/tests/static-runtime.c"
  "${ROTMOS_ROOT}/tests/storage-filesystem.c" "${ROTMOS_ROOT}/tests/webdav-runtime.c"
  "${ROTMOS_ROOT}/tests/source-runtime.c" "${ROTMOS_ROOT}/tests/upload-runtime.c" "${ROTMOS_ROOT}/tests/rest-runtime.c" "${ROTMOS_ROOT}/tests/openapi.c" "${ROTMOS_ROOT}/tests/policy-runtime.c"
  "${ROTMOS_ROOT}/tests/policy-logs-runtime.c"
  DESTINATION "${TEST_ROOT}/source")
include("${ROTMOS_ROOT}/cmake/Version.cmake")
rotmos_version("${ROTMOS_ROOT}" version)
function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE diagnostic)
  file(APPEND "${TEST_ROOT}/prepare.log" "${ARGV}\n${log}${diagnostic}\n")
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
file(COPY "${TEST_ROOT}/installed/" DESTINATION "${TEST_ROOT}/relocated SDK")
# Retain consumer objects and unchanged SDK timestamps, while making the original
# install location unavailable throughout configure/link/execution.
file(REMOVE_RECURSE "${TEST_ROOT}/unavailable")
file(RENAME "${TEST_ROOT}/installed" "${TEST_ROOT}/unavailable")
foreach(variant static shared)
  run("${CMAKE_COMMAND}" -S "${TEST_ROOT}/source" -B "${TEST_ROOT}/${variant}" -G Ninja
    "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}" "-DROTMOS_TARGET=${ROTMOS_TARGET}"
    "-DROTMOS_PREFIX=${TEST_ROOT}/relocated SDK" "-DROTMOS_VARIANT=${variant}"
    "-DROTMOS_VERSION=${version}")
  run("${CMAKE_COMMAND}" --build "${TEST_ROOT}/${variant}" --parallel 1)
endforeach()
