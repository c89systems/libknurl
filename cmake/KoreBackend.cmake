set(kore_generated "${CMAKE_CURRENT_BINARY_DIR}/kore-generated")
file(MAKE_DIRECTORY "${kore_generated}")
file(CONFIGURE OUTPUT "${kore_generated}/version.c" CONTENT
  "const char *kore_version = \"librotmos\";\nconst char *kore_build_date = \"@PROJECT_VERSION@\";\n" @ONLY)
set(kore_names kore buf connection domain filemap fileref json log mem
  msg module net pool runtime sha1 sha2 timer utils worker tls_openssl
  keymgr_openssl auth accesslog http route validator websocket acme curl ljson)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  list(APPEND kore_names linux seccomp)
  add_custom_command(OUTPUT "${kore_generated}/platform.h"
    COMMAND "${CMAKE_COMMAND}" "-DKORE_SOURCE=${kore_source_dir}"
      "-DTARGET_PLATFORM=${CMAKE_SYSTEM_PROCESSOR}" "-DOUTPUT_DIRECTORY=${kore_generated}"
      -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/GenerateKorePlatform.cmake"
    DEPENDS "${kore_source_dir}/.patched" cmake/GenerateKorePlatform.cmake VERBATIM)
else()
  list(APPEND kore_names bsd)
  file(WRITE "${kore_generated}/platform.h" "")
endif()
set(kore_objects "${kore_generated}/version.c" "${kore_generated}/platform.h")
foreach(name IN LISTS kore_names)
  list(APPEND kore_objects "${kore_source_dir}/src/${name}.c")
endforeach()
set_source_files_properties(${kore_objects} PROPERTIES GENERATED TRUE)
rotmos_kore_object_dependencies("${kore_source_dir}/.patched" ${kore_objects})
add_library(rotmos_kore OBJECT ${kore_objects})
add_dependencies(rotmos_kore kore-source)
target_include_directories(rotmos_kore PRIVATE "${kore_source_dir}/include/kore" "${kore_generated}")
target_link_libraries(rotmos_kore PRIVATE OpenSSL::SSL CURL::libcurl lonejson::lonejson pslog::pslog_shared)
target_compile_definitions(rotmos_kore PRIVATE _GNU_SOURCE=1 _FILE_OFFSET_BITS=64 KORE_SINGLE_BINARY
  KORE_USE_ACME KORE_USE_CURL KORE_USE_LONEJSON KORE_USE_PSLOG
  KORE_ROTMOS_STATIC_RUNTIME KORE_ROTMOS_DIRECT_TLS
  KORE_ROTMOS_NO_SECCOMP TLS_BACKEND_OPENSSL KORE_TMPDIR="/tmp" PREFIX="/usr/local")
rotmos_c89(rotmos_kore)
if(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
  target_compile_definitions(rotmos_kore PRIVATE _DARWIN_C_SOURCE=1)
endif()
# Callback signatures retain upstream's intentionally unused parameters.
target_compile_options(rotmos_kore PRIVATE -Wno-unused-parameter)
