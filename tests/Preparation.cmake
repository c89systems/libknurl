cmake_minimum_required(VERSION 3.21)
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/source/src" "${TEST_ROOT}/patches")
file(WRITE "${TEST_ROOT}/source/src/kore.c" "upstream\n")
set(first "--- a/src/kore.c\n+++ b/src/kore.c\n@@ -1 +1 @@\n-upstream\n+first\n")
set(second "--- a/src/kore.c\n+++ b/src/kore.c\n@@ -1 +1 @@\n-first\n+second\n")
file(WRITE "${TEST_ROOT}/patches/first.patch" "${first}")
file(WRITE "${TEST_ROOT}/patches/second.patch" "${second}")
set(output "${TEST_ROOT}/prepared")

function(prepare expect_success expected_error)
  execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DKORE_SOURCE=${TEST_ROOT}/source"
    "-DKORE_PATCHES=${TEST_ROOT}/patches"
    "-DKORE_OUTPUT=${output}"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    -P "${KNURL_ROOT}/cmake/PrepareKore.cmake"
    RESULT_VARIABLE result OUTPUT_VARIABLE log ERROR_VARIABLE error)
  if(expect_success)
    if(NOT result EQUAL 0 OR NOT EXISTS "${output}/.patched")
      message(FATAL_ERROR "Preparation failed: ${log}${error}")
    endif()
  elseif(result EQUAL 0 OR NOT "${log}${error}" MATCHES "${expected_error}")
    message(FATAL_ERROR "Expected ${expected_error}; got result=${result}: ${log}${error}")
  elseif(EXISTS "${output}/.patched" OR EXISTS "${output}.staging")
    message(FATAL_ERROR "Failed preparation left a success stamp or partial staging tree")
  endif()
endfunction()

function(assert_file path expected)
  file(READ "${path}" actual)
  if(NOT actual STREQUAL expected)
    message(FATAL_ERROR "Unexpected contents of ${path}: ${actual}")
  endif()
endfunction()

# Comments, empty lines, order, repeatability and stale-output removal.
file(WRITE "${TEST_ROOT}/patches/series" "# ordered\n\nfirst.patch\nsecond.patch\n")
prepare(TRUE "")
assert_file("${output}/src/kore.c" "second\n")
assert_file("${TEST_ROOT}/source/src/kore.c" "upstream\n")
file(WRITE "${output}/stale" "obsolete generated file\n")
prepare(TRUE "")
if(EXISTS "${output}/stale")
  message(FATAL_ERROR "Preparation retained obsolete output")
endif()

# Removing a patch replays from upstream instead of leaving its edits in place.
file(WRITE "${TEST_ROOT}/patches/series" "first.patch\n")
prepare(TRUE "")
assert_file("${output}/src/kore.c" "first\n")

# A failed upgrade never publishes partly applied sources or a successful stamp.
file(WRITE "${TEST_ROOT}/patches/series" "second.patch\nfirst.patch\n")
prepare(FALSE "Kore patch failed: second.patch")
assert_file("${output}/src/kore.c" "first\n")
file(WRITE "${TEST_ROOT}/patches/series" "missing.patch\n")
prepare(FALSE "Missing Kore patch")
file(WRITE "${TEST_ROOT}/patches/series" "first.patch\nfirst.patch\n")
prepare(FALSE "Duplicate patch")
file(WRITE "${TEST_ROOT}/patches/series" "../first.patch\n")
prepare(FALSE "Invalid patch filename")
file(WRITE "${TEST_ROOT}/patches/series" "first.patch\nsecond.patch\n")
prepare(TRUE "")
assert_file("${output}/src/kore.c" "second\n")

# Cleanup must refuse the vendor snapshot and the build root itself.
foreach(refused "${KNURL_ROOT}/vendor/kore" "${KNURL_ROOT}/build")
  execute_process(COMMAND "${CMAKE_COMMAND}"
    "-DKORE_SOURCE=${TEST_ROOT}/source"
    "-DKORE_PATCHES=${TEST_ROOT}/patches"
    "-DKORE_OUTPUT=${refused}" -P "${KNURL_ROOT}/cmake/PrepareKore.cmake"
    RESULT_VARIABLE result ERROR_VARIABLE error OUTPUT_QUIET)
  if(result EQUAL 0 OR NOT error MATCHES "KORE_OUTPUT must be a directory below")
    message(FATAL_ERROR "Unsafe output was not rejected: ${refused}: ${error}")
  endif()
endforeach()
