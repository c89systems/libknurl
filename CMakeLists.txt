cmake_minimum_required(VERSION 3.24)
include(cmake/OwnedPaths.cmake)
rotmos_require_owned_path("${CMAKE_BINARY_DIR}" "Build directory")
if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux" AND NOT CMAKE_TOOLCHAIN_FILE)
  message(FATAL_ERROR "Linux builds require the pinned toolchain; use make build or a CMake preset")
endif()
file(STRINGS VERSION rotmos_version LIMIT_COUNT 1)
project(rotmos VERSION "${rotmos_version}" LANGUAGES C CXX)
include(CTest)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C90_STANDARD_COMPILE_OPTION "-std=c89")
set(ROTMOS_TARGET "arm64-apple-darwin" CACHE STRING "Selected SDK/toolchain target")
include(cmake/Dependencies.cmake)

function(rotmos_c89 target)
  set_target_properties(${target} PROPERTIES C_STANDARD 90 C_STANDARD_REQUIRED YES
    C_EXTENSIONS NO POSITION_INDEPENDENT_CODE YES C_VISIBILITY_PRESET hidden)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror
    "$<$<COMPILE_LANGUAGE:C>:-std=c89;-Wstrict-prototypes;-Wmissing-prototypes>"
    "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
    "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
endfunction()

function(rotmos_local_runtime target)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    file(GLOB loaders "${CMAKE_SYSROOT}/lib/ld-linux*.so*" "${CMAKE_SYSROOT}/lib/ld-musl*.so*")
    list(LENGTH loaders loader_count)
    if(NOT loader_count EQUAL 1)
      message(FATAL_ERROR "Expected one selected runtime loader in ${CMAKE_SYSROOT}/lib")
    endif()
    list(GET loaders 0 loader)
    target_link_options(${target} PRIVATE "-Wl,--dynamic-linker,${loader}" -Wl,--disable-new-dtags)
    set_target_properties(${target} PROPERTIES BUILD_RPATH
      "${CMAKE_SYSROOT}/lib;${CMAKE_SYSROOT}/usr/lib;${ROTMOS_SDK_ROOT}/cpkt/lib;${ROTMOS_SDK_ROOT}/lonejson/lib;${ROTMOS_SDK_ROOT}/pslog/lib")
  endif()
endfunction()

add_library(rotmos_objects OBJECT src/common.c src/u64.c src/sse.c)
target_include_directories(rotmos_objects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(rotmos_objects PRIVATE ROTMOS_VERSION="${PROJECT_VERSION}")
rotmos_c89(rotmos_objects)
foreach(variant static shared)
  string(TOUPPER "${variant}" kind)
  add_library(rotmos_${variant} ${kind} $<TARGET_OBJECTS:rotmos_objects>)
  add_library(rotmos::${variant} ALIAS rotmos_${variant})
  set_target_properties(rotmos_${variant} PROPERTIES OUTPUT_NAME rotmos
    EXPORT_NAME ${variant} VERSION ${PROJECT_VERSION} SOVERSION 0)
  target_include_directories(rotmos_${variant} PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include> $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
  install(TARGETS rotmos_${variant} EXPORT rotmosTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR} LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
endforeach()
set(rotmos_core_headers include/rotmos/common.h include/rotmos/http.h include/rotmos/rotmos.h)
set_source_files_properties(${rotmos_core_headers} PROPERTIES LANGUAGE C HEADER_FILE_ONLY FALSE)
add_library(rotmos_headers_c OBJECT ${rotmos_core_headers})
target_include_directories(rotmos_headers_c PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include")
rotmos_c89(rotmos_headers_c)
install(DIRECTORY include/rotmos DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
install(FILES LICENSE DESTINATION ${CMAKE_INSTALL_DATADIR}/licenses/rotmos)
install(EXPORT rotmosTargets NAMESPACE rotmos:: DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rotmos)
configure_package_config_file(cmake/rotmosConfig.cmake.in "${CMAKE_CURRENT_BINARY_DIR}/rotmosConfig.cmake"
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rotmos)
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/rotmosConfigVersion.cmake"
  VERSION ${PROJECT_VERSION} COMPATIBILITY ExactVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rotmosConfig.cmake"
  "${CMAKE_CURRENT_BINARY_DIR}/rotmosConfigVersion.cmake" DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/rotmos)
configure_file(cmake/rotmos.pc.in "${CMAKE_CURRENT_BINARY_DIR}/rotmos.pc" @ONLY)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/rotmos.pc" DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig)

include(cmake/KoreSources.cmake)
set(kore_source_dir "${CMAKE_CURRENT_BINARY_DIR}/kore")
rotmos_add_kore_sources("${CMAKE_CURRENT_SOURCE_DIR}" "${kore_source_dir}")
include(cmake/KoreBackend.cmake)

get_filename_component(rotmos_toolchain "${CMAKE_TOOLCHAIN_FILE}" ABSOLUTE BASE_DIR "${CMAKE_CURRENT_SOURCE_DIR}")
set(rotmos_can_run FALSE)
if(CMAKE_HOST_SYSTEM_NAME STREQUAL CMAKE_SYSTEM_NAME AND CMAKE_HOST_SYSTEM_PROCESSOR STREQUAL CMAKE_SYSTEM_PROCESSOR)
  set(rotmos_can_run TRUE)
endif()

if(BUILD_TESTING)
  add_executable(rotmos_kore_port tests/kore-port.c "${kore_source_dir}/src/sha2.c")
  add_dependencies(rotmos_kore_port kore-source)
  target_compile_definitions(rotmos_kore_port PRIVATE _GNU_SOURCE=1 TLS_BACKEND_OPENSSL)
  target_include_directories(rotmos_kore_port PRIVATE "${kore_source_dir}/include/kore")
  target_link_libraries(rotmos_kore_port PRIVATE OpenSSL::SSL)
  rotmos_c89(rotmos_kore_port)
  rotmos_local_runtime(rotmos_kore_port)
  add_test(NAME rotmos.kore-port COMMAND rotmos_kore_port)
  add_executable(rotmos_foundation tests/foundation.c)
  target_link_libraries(rotmos_foundation PRIVATE rotmos::shared)
  rotmos_c89(rotmos_foundation)
  rotmos_local_runtime(rotmos_foundation)
  add_test(NAME rotmos.foundation COMMAND rotmos_foundation)
  add_executable(rotmos_headers_cpp tests/headers.cpp)
  target_link_libraries(rotmos_headers_cpp PRIVATE rotmos::shared)
  set_target_properties(rotmos_headers_cpp PROPERTIES CXX_STANDARD 98 CXX_STANDARD_REQUIRED YES CXX_EXTENSIONS NO)
  target_compile_options(rotmos_headers_cpp PRIVATE -Wall -Wextra -Wpedantic -Werror)
  rotmos_local_runtime(rotmos_headers_cpp)
  add_test(NAME rotmos.headers.cpp COMMAND rotmos_headers_cpp)
  find_package(Git REQUIRED)
  add_test(NAME rotmos.installed-consumer COMMAND "${CMAKE_COMMAND}"
    "-DROTMOS_ROOT=${CMAKE_CURRENT_SOURCE_DIR}" "-DROTMOS_BINARY=${CMAKE_CURRENT_BINARY_DIR}"
    "-DTEST_ROOT=${CMAKE_CURRENT_BINARY_DIR}/tests/consumer"
    "-DTOOLCHAIN=${rotmos_toolchain}" "-DROTMOS_TARGET=${ROTMOS_TARGET}"
    "-DROTMOS_CAN_RUN=${rotmos_can_run}"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/InstalledConsumer.cmake")
  set_tests_properties(rotmos.installed-consumer PROPERTIES TIMEOUT 60)
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
