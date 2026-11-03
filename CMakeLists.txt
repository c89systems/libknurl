cmake_minimum_required(VERSION 3.21)

# This bootstrap prepares the backend sources. No server library is built yet.
project(librotmos LANGUAGES NONE)

find_package(Git REQUIRED)
include(CTest)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/kore.revision" kore_revision)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/kore.revision")
string(LENGTH "${kore_revision}" kore_revision_length)
if(NOT kore_revision MATCHES "^[0-9a-f]+$" OR NOT kore_revision_length EQUAL 40)
  message(FATAL_ERROR "vendor/kore.revision must contain one full Git commit SHA")
endif()

file(GLOB_RECURSE kore_inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
  "${CMAKE_CURRENT_SOURCE_DIR}/vendor/kore/*"
  "${CMAKE_CURRENT_SOURCE_DIR}/patches/kore/*.patch")
set(kore_source_dir "${CMAKE_CURRENT_BINARY_DIR}/kore")
add_custom_command(
  OUTPUT "${kore_source_dir}/.patched"
  COMMAND "${CMAKE_COMMAND}"
    "-DKORE_SOURCE=${CMAKE_CURRENT_SOURCE_DIR}/vendor/kore"
    "-DKORE_PATCHES=${CMAKE_CURRENT_SOURCE_DIR}/patches/kore"
    "-DKORE_OUTPUT=${kore_source_dir}"
    "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/PrepareKore.cmake"
  DEPENDS ${kore_inputs}
    vendor/kore.revision vendor/kore.url patches/kore/series
    cmake/PrepareKore.cmake
  COMMENT "Preparing Kore ${kore_revision} with the ordered librotmos patch stack"
  VERBATIM)
add_custom_target(kore-source ALL DEPENDS "${kore_source_dir}/.patched")

if(BUILD_TESTING)
  add_test(NAME kore.patch-replay
    COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/patch-replay"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/PatchReplay.cmake")
  add_test(NAME kore.preparation
    COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/preparation"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/Preparation.cmake")
  add_test(NAME kore.build-invalidation
    COMMAND "${CMAKE_COMMAND}"
      "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}"
      "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/build invalidation"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/BuildInvalidation.cmake")
  find_program(BASH_EXECUTABLE bash REQUIRED)
  add_test(NAME kore.upgrade
    COMMAND "${BASH_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/upgrade-kore.sh"
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_BINARY_DIR}/tests/upgrade path")
  set_tests_properties(kore.patch-replay kore.preparation kore.build-invalidation
    kore.upgrade PROPERTIES TIMEOUT 60)
endif()
