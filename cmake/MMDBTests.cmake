find_program(KNURL_FIXTURE_PYTHON python3 REQUIRED)
set(mmdb_fixtures "${CMAKE_CURRENT_BINARY_DIR}/tests/mmdb-fixtures")
add_custom_command(OUTPUT "${mmdb_fixtures}/ready"
  COMMAND "${KNURL_FIXTURE_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/mmdb-fixtures.py" "${mmdb_fixtures}"
  COMMAND "${CMAKE_COMMAND}" -E touch "${mmdb_fixtures}/ready"
  DEPENDS tests/mmdb-fixtures.py VERBATIM)
add_custom_target(knurl_mmdb_fixtures DEPENDS "${mmdb_fixtures}/ready")
add_executable(knurl_mmdb_test tests/mmdb.c)
target_link_libraries(knurl_mmdb_test PRIVATE knurl::shared Threads::Threads)
knurl_c89(knurl_mmdb_test)
knurl_local_runtime(knurl_mmdb_test)
add_dependencies(knurl_mmdb_test knurl_mmdb_fixtures)
add_test(NAME knurl.mmdb COMMAND knurl_mmdb_test "${mmdb_fixtures}")
set_tests_properties(knurl.mmdb PROPERTIES TIMEOUT 30)
add_executable(knurl_mmdb_benchmark tests/mmdb-benchmark.c)
target_link_libraries(knurl_mmdb_benchmark PRIVATE knurl::shared)
target_compile_definitions(knurl_mmdb_benchmark PRIVATE
  $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
knurl_c89(knurl_mmdb_benchmark)
knurl_local_runtime(knurl_mmdb_benchmark)
add_executable(knurl_mmdb_benchmark_static tests/mmdb-benchmark.c)
target_link_libraries(knurl_mmdb_benchmark_static PRIVATE knurl::static)
target_compile_definitions(knurl_mmdb_benchmark_static PRIVATE
  $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
knurl_c89(knurl_mmdb_benchmark_static)
knurl_local_runtime(knurl_mmdb_benchmark_static)
add_executable(knurl_mmdb_corpus tests/mmdb-corpus.c)
target_link_libraries(knurl_mmdb_corpus PRIVATE knurl::shared)
knurl_c89(knurl_mmdb_corpus)
knurl_local_runtime(knurl_mmdb_corpus)
add_dependencies(knurl_mmdb_corpus knurl_mmdb_fixtures)
add_test(NAME knurl.mmdb.corpus COMMAND knurl_mmdb_corpus
  "${mmdb_fixtures}/valid-28-6.mmdb" 65536)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  add_executable(knurl_mmdb_memory tests/mmdb-memory.c)
  target_link_libraries(knurl_mmdb_memory PRIVATE knurl::static)
  target_compile_definitions(knurl_mmdb_memory PRIVATE _FILE_OFFSET_BITS=64)
  if(KNURL_TARGET MATCHES "-linux-gnu$")
    target_compile_definitions(knurl_mmdb_memory PRIVATE KNURL_MMDB_PREAD64=1)
    set(mmdb_io_wrappers -Wl,--wrap=pread64 -Wl,--wrap=mmap64)
  else()
    set(mmdb_io_wrappers -Wl,--wrap=pread -Wl,--wrap=mmap)
  endif()
  target_link_options(knurl_mmdb_memory PRIVATE
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free
    ${mmdb_io_wrappers})
  add_executable(knurl_mmdb_benchmark_probe tests/mmdb-benchmark.c tests/mmdb-memory.c)
  target_compile_definitions(knurl_mmdb_benchmark_probe PRIVATE
    KNURL_MMDB_BENCHMARK_PROBE=1 _FILE_OFFSET_BITS=64)
  if(KNURL_TARGET MATCHES "-linux-gnu$")
    target_compile_definitions(knurl_mmdb_benchmark_probe PRIVATE KNURL_MMDB_PREAD64=1)
  endif()
  target_link_libraries(knurl_mmdb_benchmark_probe PRIVATE knurl::static)
  target_link_options(knurl_mmdb_benchmark_probe PRIVATE
    -Wl,--wrap=knurl_mmdb_prefix_get
    -Wl,--wrap=malloc -Wl,--wrap=calloc -Wl,--wrap=realloc -Wl,--wrap=free
    ${mmdb_io_wrappers})
  knurl_c89(knurl_mmdb_benchmark_probe)
  knurl_local_runtime(knurl_mmdb_benchmark_probe)
  add_dependencies(knurl_mmdb_benchmark_probe knurl_mmdb_fixtures)
  add_test(NAME knurl.mmdb.benchmark-probe COMMAND knurl_mmdb_benchmark_probe
    "${mmdb_fixtures}/cache.mmdb" random 65536)
  add_test(NAME knurl.mmdb.benchmark-sweep COMMAND "${CMAKE_COMMAND}"
    "-DBENCHMARK=$<TARGET_FILE:knurl_mmdb_benchmark>"
    "-DPROBE=$<TARGET_FILE:knurl_mmdb_benchmark_probe>"
    "-DDATABASE=${mmdb_fixtures}/valid-28-6.mmdb"
    "-DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/tests/mmdb-sweep"
    -DHEAP_LIMITS=0 -DROUNDS=1 -DADDRESS_COUNT=257
    -P "${CMAKE_CURRENT_SOURCE_DIR}/tests/MMDBPerformanceDefaults.cmake")
  knurl_c89(knurl_mmdb_memory)
  knurl_local_runtime(knurl_mmdb_memory)
  add_dependencies(knurl_mmdb_memory knurl_mmdb_fixtures)
  foreach(limit 0 8192 65536 262144)
    add_test(NAME knurl.mmdb.memory-${limit} COMMAND knurl_mmdb_memory
      "${mmdb_fixtures}/cache.mmdb" ${limit})
  endforeach()
endif()
