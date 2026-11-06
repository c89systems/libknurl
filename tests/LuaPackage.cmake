cmake_minimum_required(VERSION 3.24)
include("${ROTMOS_ROOT}/cmake/OwnedPaths.cmake")
rotmos_require_owned_path("${TEST_ROOT}" "Lua artifact fixture")
find_program(BASH bash REQUIRED)
function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Lua artifact check failed: ${ARGV}\n${output}${errors}")
  endif()
endfunction()
file(REMOVE_RECURSE "${TEST_ROOT}")
include("${ROTMOS_ROOT}/cmake/Version.cmake")
rotmos_version("${ROTMOS_ROOT}" version)
set(package "lua-rotmos-${version}")
run("${CMAKE_COMMAND}" "-DOUTPUT_DIR=${TEST_ROOT}/products"
  "-DROTMOS_CANDIDATE_VERSION=${ROTMOS_CANDIDATE_VERSION}"
  -P "${ROTMOS_ROOT}/lua/cmake/Package.cmake")
file(STRINGS "${TEST_ROOT}/products/${package}-CHECKSUMS" checksums)
run("${CMAKE_COMMAND}" -E env TZ=Pacific/Honolulu "${BASH}" -c
  [=[umask 077 && exec "$@"]=] -- "${CMAKE_COMMAND}"
  "-DROTMOS_CANDIDATE_VERSION=${ROTMOS_CANDIDATE_VERSION}"
  "-DOUTPUT_DIR=${TEST_ROOT}/repeat" -P "${ROTMOS_ROOT}/lua/cmake/Package.cmake")
file(STRINGS "${TEST_ROOT}/repeat/${package}-CHECKSUMS" repeated)
if(NOT checksums STREQUAL repeated)
  message(FATAL_ERROR "Lua artifacts changed with staging umask/timezone")
endif()
run("${CMAKE_COMMAND}" "-DPRODUCT_DIR=${TEST_ROOT}/products"
  "-DVERIFY_DIR=${TEST_ROOT}/verified" "-DROTMOS_CANDIDATE_VERSION=${ROTMOS_CANDIDATE_VERSION}"
  -P "${ROTMOS_ROOT}/cmake/VerifyLuaPackage.cmake")
set(source "${TEST_ROOT}/verified/source/${package}")
include("${ROTMOS_ROOT}/tests/LuaPrivacy.cmake")
# Exercise rejection without altering the checkout's manifest or sources.
file(APPEND "${source}/lua/source-manifest.txt" "\n../outside\n")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${source}/build/rejected"
  -P "${source}/lua/cmake/Package.cmake" RESULT_VARIABLE rejected
  OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(rejected EQUAL 0 OR NOT errors MATCHES "Invalid Lua source-manifest input")
  message(FATAL_ERROR "Unsafe source input was not rejected: ${output}${errors}")
endif()
configure_file("${ROTMOS_ROOT}/lua/source-manifest.txt"
  "${source}/lua/source-manifest.txt" COPYONLY)
execute_process(COMMAND "${BASH}" "${source}/lua/build-rock.sh" native "${source}" "${source}"
  RESULT_VARIABLE rejected OUTPUT_VARIABLE output ERROR_VARIABLE errors)
if(rejected EQUAL 0 OR NOT errors MATCHES "unsupported target: native")
  message(FATAL_ERROR "Unselected compiler target was not rejected: ${output}${errors}")
endif()
