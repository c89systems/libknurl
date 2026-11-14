cmake_minimum_required(VERSION 3.24)
include("${KNURL_ROOT}/cmake/OwnedPaths.cmake")
knurl_require_owned_path("${TEST_ROOT}" "Ownership fixture")
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/owner/cmake" "${TEST_ROOT}/owner/build"
  "${TEST_ROOT}/borrowed/nested")
file(COPY "${KNURL_ROOT}/cmake/OwnedPaths.cmake" DESTINATION "${TEST_ROOT}/owner/cmake")
file(WRITE "${TEST_ROOT}/borrowed/victim" "preserve\n")
file(CREATE_LINK "${TEST_ROOT}/borrowed/nested" "${TEST_ROOT}/owner/build/link" SYMBOLIC)
function(probe path success)
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DKNURL_CHECK_PATH=${path}"
    -P "${TEST_ROOT}/owner/cmake/OwnedPaths.cmake"
    RESULT_VARIABLE code OUTPUT_VARIABLE log ERROR_VARIABLE errors)
  if((success AND NOT code EQUAL 0) OR (NOT success AND code EQUAL 0))
    message(FATAL_ERROR "Unexpected ownership decision for ${path}: ${log}${errors}")
  endif()
endfunction()
probe("${TEST_ROOT}/owner/build/new/path" TRUE)
probe("${TEST_ROOT}/owner/build/" TRUE)
probe("${TEST_ROOT}/owner/build/ordinary/../new" TRUE)
probe("${TEST_ROOT}/borrowed/victim" FALSE)
probe("${TEST_ROOT}/owner/build/link/missing" FALSE)
probe("${TEST_ROOT}/owner/build/link/../victim" FALSE)
probe("${TEST_ROOT}/owner/build/link/../missing" FALSE)
probe("${TEST_ROOT}/owner/build/../build/link/../victim" FALSE)
file(READ "${TEST_ROOT}/borrowed/victim" victim)
if(NOT victim STREQUAL "preserve\n")
  message(FATAL_ERROR "Ownership probe modified borrowed bytes")
endif()
# Fixture installation must validate its destination before CMake can write it.
file(MAKE_DIRECTORY "${TEST_ROOT}/owner/lua/cmake" "${TEST_ROOT}/owner/build/lua")
file(COPY "${KNURL_ROOT}/lua/cmake/RunFixture.cmake" DESTINATION "${TEST_ROOT}/owner/lua/cmake")
file(WRITE "${TEST_ROOT}/owner/build/lua/cmake_install.cmake"
  "file(WRITE \"\${CMAKE_INSTALL_PREFIX}/installed\" \"must not write borrowed inputs\")\n")
execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DBINARY=${TEST_ROOT}/owner/build/lua" "-DPREFIX=${TEST_ROOT}/borrowed"
  -P "${TEST_ROOT}/owner/lua/cmake/RunFixture.cmake"
  RESULT_VARIABLE code OUTPUT_VARIABLE log ERROR_VARIABLE errors)
if(code EQUAL 0 OR NOT "${log}${errors}" MATCHES "must use this checkout" OR
    EXISTS "${TEST_ROOT}/borrowed/installed")
  message(FATAL_ERROR "Lua fixture wrote before rejecting its borrowed prefix: ${log}${errors}")
endif()
