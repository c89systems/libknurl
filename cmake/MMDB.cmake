# Pristine embedded vendor sources; no host discovery or consumer dependency.
set(mmdb_root "${CMAKE_CURRENT_SOURCE_DIR}/vendor/libmaxminddb")
set(MMDB_UINT128_USING_MODE 0)
set(MMDB_UINT128_IS_BYTE_ARRAY 1)
configure_file("${mmdb_root}/include/maxminddb_config.h.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/mmdb/maxminddb_config.h")
set(mmdb_symbols MMDB_open MMDB_lookup_string MMDB_lookup_sockaddr MMDB_read_node
  MMDB_get_value MMDB_vget_value MMDB_aget_value MMDB_get_metadata_as_entry_data_list
  MMDB_get_entry_data_list MMDB_free_entry_data_list MMDB_close MMDB_lib_version
  MMDB_dump_entry_data_list MMDB_strerror can_multiply data_pool_new
  data_pool_destroy data_pool_alloc data_pool_to_list)
set(mmdb_prefixes "")
foreach(symbol IN LISTS mmdb_symbols)
  list(APPEND mmdb_prefixes "${symbol}=knurl_mmdb_vendor_${symbol}")
endforeach()
add_library(knurl_mmdb_vendor OBJECT "${mmdb_root}/src/maxminddb.c"
  "${mmdb_root}/src/data-pool.c")
set_target_properties(knurl_mmdb_vendor PROPERTIES C_STANDARD 99
  C_STANDARD_REQUIRED YES C_EXTENSIONS NO POSITION_INDEPENDENT_CODE YES
  C_VISIBILITY_PRESET hidden)
target_include_directories(knurl_mmdb_vendor PRIVATE "${mmdb_root}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/mmdb")
target_compile_definitions(knurl_mmdb_vendor PRIVATE ${mmdb_prefixes} PACKAGE_VERSION="1.14.1")
target_compile_options(knurl_mmdb_vendor PRIVATE
  "-ffile-prefix-map=${CMAKE_SOURCE_DIR}=."
  "-ffile-prefix-map=${CMAKE_BINARY_DIR}=build")
target_include_directories(knurl_objects SYSTEM PRIVATE "${mmdb_root}/include"
  "${CMAKE_CURRENT_BINARY_DIR}/mmdb")
set_source_files_properties(src/mmdb_maxmind.c PROPERTIES COMPILE_DEFINITIONS "${mmdb_prefixes}")
install(FILES "${mmdb_root}/LICENSE" "${mmdb_root}/NOTICE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/knurl/libmaxminddb")
