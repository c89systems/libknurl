cmake_minimum_required(VERSION 3.24)
include("${KNURL_ROOT}/cmake/OwnedPaths.cmake")
include("${KNURL_ROOT}/cmake/Privacy.cmake")
knurl_require_owned_path("${TEST_ROOT}" "LuaRocks consumer")
find_program(LUAROCKS luarocks REQUIRED)
include("${KNURL_ROOT}/cmake/Version.cmake")
knurl_version("${KNURL_ROOT}" version)
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/tmp" "${TEST_ROOT}/tree")
if(NOT PRODUCT_DIR)
  set(PRODUCT_DIR "${TEST_ROOT}/products")
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${PRODUCT_DIR}"
    "-DKNURL_CANDIDATE_VERSION=${KNURL_CANDIDATE_VERSION}"
    -P "${KNURL_ROOT}/lua/cmake/Package.cmake" RESULT_VARIABLE result
    OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Lua source packaging failed: ${output}${errors}")
  endif()
endif()
file(WRITE "${TEST_ROOT}/config.lua"
  "local_cache = [=[${TEST_ROOT}/cache]=]\nrocks_trees = { [=[${TEST_ROOT}/tree]=] }\n")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env
  "LUAROCKS_CONFIG=${TEST_ROOT}/config.lua" "TMPDIR=${TEST_ROOT}/tmp"
  "${LUAROCKS}" --no-project --lua-version=5.5 "--tree=${TEST_ROOT}/tree"
  build --deps-mode=none "${PRODUCT_DIR}/lua-knurl-${version}-1.src.rock"
  "LUA_INCDIR=${KNURL_SDK_ROOT}/cpkt/include" "KNURL_TARGET=${KNURL_TARGET}"
  "KNURL_CORE_PREFIX=${KNURL_CORE_PREFIX}" "KNURL_SDK_ROOT=${KNURL_SDK_ROOT}"
  WORKING_DIRECTORY "${TEST_ROOT}" RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${TEST_ROOT}/build.log" "${output}${errors}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "LuaRocks source consumer failed: ${output}${errors}")
endif()
set(module "${TEST_ROOT}/tree/lib/lua/5.5/knurl/native.so")
foreach(product "${module}" "${TEST_ROOT}/tree/share/lua/5.5/knurl.lua"
    "${TEST_ROOT}/tree/share/lua/5.5/knurl/meta.lua"
    "${TEST_ROOT}/tree/lib/luarocks/rocks-5.5/lua-knurl/${version}-1/doc/LICENSE")
  if(NOT EXISTS "${product}")
    message(FATAL_ERROR "Missing installed LuaRocks product: ${product}")
  endif()
  knurl_verify_private_bytes("${product}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
  "${KNURL_ROOT}/lua/knurl.meta.lua"
  "${TEST_ROOT}/tree/share/lua/5.5/knurl/meta.lua" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed LuaRocks editor metadata differs from the shipped source")
endif()
if(HOST)
  execute_process(COMMAND "${HOST}" "${KNURL_ROOT}/tests/lua/foundation.lua"
    "${TEST_ROOT}/tree" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed LuaRocks runtime failed: ${output}${errors}")
  endif()
endif()
