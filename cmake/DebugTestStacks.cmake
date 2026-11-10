# Include only in projects building processes under test. SDK libraries and
# CMake/Ninja themselves never link this startup code.
include_guard(GLOBAL)
set(knurl_test_stack_source "${CMAKE_CURRENT_LIST_DIR}/../tests/debug-thread-stack.c")
function(knurl_configure_debug_test_stacks)
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug" OR NOT KNURL_TARGET MATCHES "-linux-gnu$")
    return()
  endif()
  get_property(targets DIRECTORY PROPERTY BUILDSYSTEM_TARGETS)
  foreach(target IN LISTS targets)
    get_target_property(kind "${target}" TYPE)
    get_target_property(control "${target}" KNURL_STACK_CONTROL)
    if(kind STREQUAL "EXECUTABLE" AND NOT control)
      if(NOT TARGET Threads::Threads)
        find_package(Threads REQUIRED)
      endif()
      target_sources("${target}" PRIVATE "${knurl_test_stack_source}")
      target_link_libraries("${target}" PRIVATE Threads::Threads)
      target_compile_definitions("${target}" PRIVATE KNURL_DEBUG_TEST_THREADS=1)
      set_property(TARGET "${target}" PROPERTY KNURL_CONSTRAINED_TEST_STACK TRUE)
    endif()
  endforeach()
  get_property(tests DIRECTORY PROPERTY TESTS)
  if(tests)
    # Tunables are consumed by the loader, before startup code executes.
    # Append our setting while retaining unrelated inherited tunables.
    set_property(TEST ${tests} APPEND PROPERTY ENVIRONMENT_MODIFICATION
      "GLIBC_TUNABLES=string_append::glibc.pthread.stack_cache_size=0")
  endif()
endfunction()
set_source_files_properties("${knurl_test_stack_source}" PROPERTIES COMPILE_OPTIONS
  "-std=c89;-Wall;-Wextra;-Wpedantic;-Werror;-Wstrict-prototypes;-Wmissing-prototypes;-Wvla;-Walloca")
cmake_language(DEFER CALL knurl_configure_debug_test_stacks)
