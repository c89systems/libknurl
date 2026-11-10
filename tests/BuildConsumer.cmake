cmake_minimum_required(VERSION 3.24)
include("${KNURL_ROOT}/cmake/OwnedPaths.cmake")
knurl_require_owned_path("${TEST_ROOT}" "Consumer fixture")
if(NOT VARIANT MATCHES "^(static|shared)$")
  message(FATAL_ERROR "Consumer build requires VARIANT=static or shared")
endif()
include("${KNURL_ROOT}/cmake/Version.cmake")
knurl_version("${KNURL_ROOT}" version)
file(WRITE "${TEST_ROOT}/${VARIANT}-build.log" "")
function(run)
  execute_process(COMMAND ${ARGV} RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE diagnostic)
  file(APPEND "${TEST_ROOT}/${VARIANT}-build.log" "${ARGV}\n${log}${diagnostic}\n")
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer build failed: ${ARGV}\n${log}${diagnostic}")
  endif()
endfunction()
run("${CMAKE_COMMAND}" -S "${TEST_ROOT}/source" -B "${TEST_ROOT}/${VARIANT}" -G Ninja
  "-DCMAKE_TOOLCHAIN_FILE=${TOOLCHAIN}" "-DKNURL_TARGET=${KNURL_TARGET}"
  "-DCMAKE_BUILD_TYPE=${KNURL_TEST_BUILD_TYPE}"
  "-DCMAKE_PROJECT_INCLUDE=${KNURL_ROOT}/cmake/DebugTestStacks.cmake"
  "-DKNURL_PREFIX=${TEST_ROOT}/relocated SDK" "-DKNURL_VARIANT=${VARIANT}"
  "-DKNURL_VERSION=${version}")
run("${CMAKE_COMMAND}" --build "${TEST_ROOT}/${VARIANT}" --parallel 1)
