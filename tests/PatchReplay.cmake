cmake_minimum_required(VERSION 3.21)

# Inspect the real imported stack, not a mock stack or a list of feature names.
file(GLOB_RECURSE originals LIST_DIRECTORIES FALSE "${KNURL_ROOT}/vendor/kore/*")
set(before)
foreach(path IN LISTS originals)
  file(SHA256 "${path}" digest)
  list(APPEND before "${digest}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}"
  "-DKORE_SOURCE=${KNURL_ROOT}/vendor/kore"
  "-DKORE_PATCHES=${KNURL_ROOT}/patches/kore"
  "-DKORE_OUTPUT=${TEST_ROOT}/kore"
  "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
  -P "${KNURL_ROOT}/cmake/PrepareKore.cmake"
  RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE error)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Real Kore patch replay failed:\n${log}${error}")
endif()
set(after)
foreach(path IN LISTS originals)
  file(SHA256 "${path}" digest)
  list(APPEND after "${digest}")
endforeach()
file(GLOB_RECURSE remaining LIST_DIRECTORIES FALSE "${KNURL_ROOT}/vendor/kore/*")
if(NOT before STREQUAL after OR NOT originals STREQUAL remaining)
  message(FATAL_ERROR "Patch preparation modified the pristine Kore snapshot")
endif()
foreach(added include/kore/ljson.h src/ljson.c src/vectis_pslog_impl.c .patched)
  if(NOT EXISTS "${TEST_ROOT}/kore/${added}")
    message(FATAL_ERROR "Patch replay did not produce ${added}")
  endif()
endforeach()
file(READ "${TEST_ROOT}/kore/src/http.c" patched_http)
file(READ "${KNURL_ROOT}/vendor/kore/src/http.c" pristine_http)
if(patched_http STREQUAL pristine_http)
  message(FATAL_ERROR "Real patch replay left the HTTP implementation unchanged")
endif()
