cmake_minimum_required(VERSION 3.24)
get_filename_component(root "${CMAKE_CURRENT_LIST_DIR}/.." REALPATH)
include("${root}/cmake/OwnedPaths.cmake")
include("${root}/cmake/Version.cmake")
knurl_version("${root}" version)
set(source "${root}/build/source-package/extracted/libknurl-${version}")
knurl_require_owned_path("${source}" "Source smoke checkout")
if(NOT EXISTS "${source}/RELEASE_MANIFEST")
  message(FATAL_ERROR "Prepare the source archive with make source-package")
endif()
set(KNURL_CANDIDATE_VERSION "")
knurl_version("${source}" extracted_version)
if(NOT extracted_version STREQUAL version)
  message(FATAL_ERROR "Source smoke version mismatch")
endif()
find_program(MAKE make REQUIRED)
set(target x86_64-linux-musl)
set(build "${source}/build/${target}/release")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env --unset=MAKEFLAGS --unset=MFLAGS
  --unset=MAKEOVERRIDES --unset=CPKT_DEPENDENCY_CACHE "${MAKE}" -C "${source}"
  configure "TARGET=${target}" CONFIG=release "BUILD_DIR=build/${target}/release"
  KNURL_BINARY=ON LUA_CONFIG=OFF CANDIDATE_VERSION= COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${build}" --parallel "${JOBS}"
  --target knurl knurl_binary_test COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${CMAKE_COMMAND}" --install "${build}" --prefix "${build}/installed"
  --component binary COMMAND_ERROR_IS_FATAL ANY)
execute_process(COMMAND "${build}/installed/bin/knurl" --version
  OUTPUT_VARIABLE reported COMMAND_ERROR_IS_FATAL ANY)
file(STRINGS "${build}/SDK" sdk_version REGEX "^version=")
if(NOT reported STREQUAL "knurl ${version}\n" OR NOT sdk_version STREQUAL "version=${version}")
  message(FATAL_ERROR "Reconstructed runner and SDK must report ${version}")
endif()
execute_process(COMMAND "${CMAKE_CTEST_COMMAND}" --test-dir "${build}" --parallel 1
  --stop-on-failure --output-on-failure --no-tests=error -R "^knurl[.]binary-normal$"
  COMMAND_ERROR_IS_FATAL ANY)
