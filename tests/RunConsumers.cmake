cmake_minimum_required(VERSION 3.24)
include("${ROTMOS_ROOT}/cmake/OwnedPaths.cmake")
rotmos_require_owned_path("${TEST_ROOT}" "Installed consumer runtime")
if(NOT VARIANT MATCHES "^(static|shared)$" OR EXISTS "${TEST_ROOT}/installed")
  message(FATAL_ERROR "Consumers require a prepared, relocated static/shared SDK")
endif()
function(run surface)
  message(STATUS "Installed ${VARIANT} consumer: ${surface} ${ARGN}")
  execute_process(COMMAND "${TEST_ROOT}/${VARIANT}/${surface}_consumer" ${ARGN}
    TIMEOUT 15 RESULT_VARIABLE code OUTPUT_VARIABLE log ERROR_VARIABLE errors)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "Installed ${VARIANT} ${surface} ${ARGN}: ${code}\n${log}${errors}")
  endif()
endfunction()
foreach(surface foundation reply declarations vectis-config vectis-typed
    vectis-filesystem vectis-xml-runtime vectis-host-runtime vectis-route-runtime
    vectis-body-file vectis-request-reply vectis-services-runtime tls-runtime
    materialized-output vectis-response-head server-runtime proxy-runtime
    proxy-websocket json xml xml-mapped xml-runtime asset mime static-runtime
    storage-filesystem webdav-runtime source-runtime upload-runtime rest-runtime
    openapi policy-runtime policy-logs-runtime json-runtime)
  run("${surface}")
endforeach()
run(vectis-response-head bounded)
run(vectis-response-head plain)
run(proxy-websocket tls)
run(storage-filesystem managed)
run(rest-runtime spill)
run(json-runtime default-directory)
