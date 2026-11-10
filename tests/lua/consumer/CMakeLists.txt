cmake_minimum_required(VERSION 3.24)
project(knurl_provider_consumer LANGUAGES C)
set(CMAKE_C90_STANDARD_COMPILE_OPTION "-std=c89")
foreach(prefix "${KNURL_LUA_PREFIX}" "${KNURL_CORE_PREFIX}"
    "${KNURL_SDK_ROOT}/cpkt" "${KNURL_SDK_ROOT}/lonejson"
    "${KNURL_SDK_ROOT}/lonejson-lua" "${KNURL_SDK_ROOT}/pslog"
    "${KNURL_SDK_ROOT}/pslog-lua")
  list(APPEND CMAKE_PREFIX_PATH "${prefix}")
  list(APPEND CMAKE_FIND_ROOT_PATH "${prefix}")
endforeach()
find_package(knurl_lua ${KNURL_VERSION} EXACT CONFIG REQUIRED)
add_library(knurl_provider_fixture MODULE providers.c)
set_target_properties(knurl_provider_fixture PROPERTIES PREFIX "" SUFFIX ".so"
  C_STANDARD 90 C_STANDARD_REQUIRED YES C_EXTENSIONS NO C_VISIBILITY_PRESET hidden)
target_compile_options(knurl_provider_fixture PRIVATE -std=c89 -Wall -Wextra -Wpedantic -Werror -Wstrict-prototypes -Wmissing-prototypes)
target_compile_definitions(knurl_provider_fixture PRIVATE _GNU_SOURCE=1 $<$<PLATFORM_ID:Darwin>:_DARWIN_C_SOURCE=1>)
target_link_libraries(knurl_provider_fixture PRIVATE knurl::lua)
