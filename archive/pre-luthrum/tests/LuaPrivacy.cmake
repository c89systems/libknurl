# Included by LuaPackage.cmake after verifying the pristine products. Negative
# cases update all relevant hashes so a digest failure cannot mask a privacy bug.
set(bad "${TEST_ROOT}/bad-products")
function(reset_products)
  file(REMOVE_RECURSE "${bad}")
  file(MAKE_DIRECTORY "${bad}")
  file(COPY "${TEST_ROOT}/products/" DESTINATION "${bad}")
endfunction()
function(reject_private)
  set(records "")
  foreach(name "${package}.tar.gz" "${package}-1.rockspec" "${package}-1.src.rock")
    file(SHA256 "${bad}/${name}" digest)
    string(APPEND records "${digest}  ${name}\n")
  endforeach()
  file(WRITE "${bad}/${package}-CHECKSUMS" "${records}")
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DPRODUCT_DIR=${bad}"
    "-DVERIFY_DIR=${TEST_ROOT}/rejected" "-DKNURL_CANDIDATE_VERSION=${KNURL_CANDIDATE_VERSION}"
    -P "${KNURL_ROOT}/cmake/VerifyLuaPackage.cmake"
    RESULT_VARIABLE code OUTPUT_VARIABLE log ERROR_VARIABLE errors)
  if(code EQUAL 0 OR NOT "${log}${errors}" MATCHES "Private key/credential")
    message(FATAL_ERROR "Private Lua artifact bytes were not rejected: ${errors}")
  endif()
endfunction()
string(REPEAT A 64 body)
set(key "-----BEGIN PRIVATE KEY-----\n${body}\n-----END PRIVATE KEY-----\n")
reset_products()
file(APPEND "${bad}/${package}-1.rockspec" "\n-- github_pat_${body}\n")
reject_private()

# The rock's own metadata must be scanned, not just compared to its counterpart.
reset_products()
set(rock "${TEST_ROOT}/bad-rock")
file(MAKE_DIRECTORY "${rock}")
file(ARCHIVE_EXTRACT INPUT "${bad}/${package}-1.src.rock" DESTINATION "${rock}")
file(APPEND "${rock}/${package}-1.rockspec" "\n--[[${key}]]\n")
file(REMOVE "${bad}/${package}-1.src.rock")
run("${CMAKE_COMMAND}" -E chdir "${rock}" "${CMAKE_COMMAND}" -E tar cf
  "${bad}/${package}-1.src.rock" --format=zip "${package}-1.rockspec" "${package}.tar.gz")
reject_private()

# Put a key in a declared source file, with correct inner/outer manifests and
# an identical source tar inside the rock. Both archive levels remain coherent.
reset_products()
file(READ "${source}/lua/README.md" readme)
file(READ "${source}/RELEASE_MANIFEST" manifest)
file(SHA256 "${source}/lua/README.md" before)
file(APPEND "${source}/lua/README.md" "\n${key}")
file(SHA256 "${source}/lua/README.md" after)
string(REPLACE "${before}  lua/README.md" "${after}  lua/README.md" records "${manifest}")
file(WRITE "${source}/RELEASE_MANIFEST" "${records}")
run("${CMAKE_COMMAND}" -E chdir "${TEST_ROOT}/verified/source" "${CMAKE_COMMAND}" -E tar czf
  "${bad}/${package}.tar.gz" --format=gnutar "${package}")
file(WRITE "${source}/lua/README.md" "${readme}")
file(WRITE "${source}/RELEASE_MANIFEST" "${manifest}")
configure_file("${bad}/${package}.tar.gz" "${rock}/${package}.tar.gz" COPYONLY)
configure_file("${bad}/${package}-1.rockspec" "${rock}/${package}-1.rockspec" COPYONLY)
file(REMOVE "${bad}/${package}-1.src.rock")
run("${CMAKE_COMMAND}" -E chdir "${rock}" "${CMAKE_COMMAND}" -E tar cf
  "${bad}/${package}-1.src.rock" --format=zip "${package}-1.rockspec" "${package}.tar.gz")
reject_private()
file(REMOVE_RECURSE "${bad}" "${rock}" "${TEST_ROOT}/rejected")
