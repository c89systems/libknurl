cmake_minimum_required(VERSION 3.24)
include("${ROTMOS_ROOT}/cmake/OwnedPaths.cmake")
include("${ROTMOS_ROOT}/cmake/Privacy.cmake")
rotmos_require_owned_path("${TEST_ROOT}" "LuaRocks consumer")
find_program(LUAROCKS luarocks REQUIRED)
include("${ROTMOS_ROOT}/cmake/Version.cmake")
rotmos_version("${ROTMOS_ROOT}" version)
file(REMOVE_RECURSE "${TEST_ROOT}")
file(MAKE_DIRECTORY "${TEST_ROOT}/tmp" "${TEST_ROOT}/tree")
if(NOT PRODUCT_DIR)
  set(PRODUCT_DIR "${TEST_ROOT}/products")
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DOUTPUT_DIR=${PRODUCT_DIR}"
    "-DROTMOS_CANDIDATE_VERSION=${ROTMOS_CANDIDATE_VERSION}"
    -P "${ROTMOS_ROOT}/lua/cmake/Package.cmake" RESULT_VARIABLE result
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
  build --deps-mode=none "${PRODUCT_DIR}/lua-rotmos-${version}-1.src.rock"
  "LUA_INCDIR=${ROTMOS_SDK_ROOT}/cpkt/include" "ROTMOS_TARGET=${ROTMOS_TARGET}"
  "ROTMOS_CORE_PREFIX=${ROTMOS_CORE_PREFIX}" "ROTMOS_SDK_ROOT=${ROTMOS_SDK_ROOT}"
  WORKING_DIRECTORY "${TEST_ROOT}" RESULT_VARIABLE result
  OUTPUT_VARIABLE output ERROR_VARIABLE errors)
file(WRITE "${TEST_ROOT}/build.log" "${output}${errors}")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "LuaRocks source consumer failed: ${output}${errors}")
endif()
set(module "${TEST_ROOT}/tree/lib/lua/5.5/rotmos/native.so")
foreach(product "${module}" "${TEST_ROOT}/tree/share/lua/5.5/rotmos.lua"
    "${TEST_ROOT}/tree/share/lua/5.5/rotmos/meta.lua"
    "${TEST_ROOT}/tree/lib/luarocks/rocks-5.5/lua-rotmos/${version}-1/doc/LICENSE")
  if(NOT EXISTS "${product}")
    message(FATAL_ERROR "Missing installed LuaRocks product: ${product}")
  endif()
  rotmos_verify_private_bytes("${product}")
endforeach()
execute_process(COMMAND "${CMAKE_COMMAND}" -E compare_files
  "${ROTMOS_ROOT}/lua/rotmos.meta.lua"
  "${TEST_ROOT}/tree/share/lua/5.5/rotmos/meta.lua" RESULT_VARIABLE result)
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Installed LuaRocks editor metadata differs from the shipped source")
endif()
if(HOST)
  execute_process(COMMAND "${HOST}" "${ROTMOS_ROOT}/tests/lua/foundation.lua"
    "${TEST_ROOT}/tree" RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors)
  if(NOT result EQUAL 0)
    message(FATAL_ERROR "Installed LuaRocks runtime failed: ${output}${errors}")
  endif()
endif()
