set(runner_notices "${CMAKE_INSTALL_DATADIR}/licenses/knurl")
install(FILES docs/knurl.md DESTINATION "${CMAKE_INSTALL_DATADIR}/knurl/docs"
  COMPONENT binary EXCLUDE_FROM_ALL)
install(FILES LICENSE LICENSES/BSD-queue.txt DESTINATION "${runner_notices}"
  COMPONENT binary EXCLUDE_FROM_ALL)
install(FILES vendor/kore/LICENSE DESTINATION "${runner_notices}/kore"
  COMPONENT binary EXCLUDE_FROM_ALL)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  install(FILES LICENSES/musl.txt DESTINATION "${runner_notices}"
    COMPONENT binary EXCLUDE_FROM_ALL)
endif()
foreach(provider curl libxml2 libssh2 nghttp2 openssl zlib lua)
  install(FILES "${KNURL_SDK_ROOT}/cpkt/share/doc/cpkt/core/third_party/${provider}/LICENSE"
    DESTINATION "${runner_notices}/${provider}" COMPONENT binary EXCLUDE_FROM_ALL)
endforeach()
foreach(provider pslog lonejson)
  install(FILES "${KNURL_SDK_ROOT}/${provider}/share/doc/lib${provider}/LICENSE"
    DESTINATION "${runner_notices}/${provider}" COMPONENT binary EXCLUDE_FROM_ALL)
endforeach()
configure_file("${CMAKE_CURRENT_SOURCE_DIR}/binary/Build.cmake.in"
  "${CMAKE_CURRENT_BINARY_DIR}/runner-build.cmake" @ONLY)
