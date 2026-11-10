function(knurl_add_kore_sources root output)
  find_package(Git REQUIRED)
  file(STRINGS "${root}/vendor/kore.revision" revision)
  string(LENGTH "${revision}" length)
  if(NOT revision MATCHES "^[0-9a-f]+$" OR NOT length EQUAL 40)
    message(FATAL_ERROR "vendor/kore.revision must contain a full Git SHA")
  endif()
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${root}/vendor/kore.revision")
  file(GLOB_RECURSE inputs CONFIGURE_DEPENDS LIST_DIRECTORIES FALSE
    "${root}/vendor/kore/*" "${root}/patches/kore/*.patch")
  add_custom_command(OUTPUT "${output}/.patched"
    COMMAND "${CMAKE_COMMAND}" "-DKORE_SOURCE=${root}/vendor/kore"
      "-DKORE_PATCHES=${root}/patches/kore" "-DKORE_OUTPUT=${output}"
      "-DGIT_EXECUTABLE=${GIT_EXECUTABLE}" -P "${root}/cmake/PrepareKore.cmake"
    DEPENDS ${inputs} "${root}/vendor/kore.revision" "${root}/vendor/kore.url"
      "${root}/patches/kore/series" "${root}/cmake/PrepareKore.cmake"
      "${root}/cmake/OwnedPaths.cmake"
    COMMENT "Preparing pristine Kore ${revision} with external patches" VERBATIM)
  add_custom_target(kore-source ALL DEPENDS "${output}/.patched")
endfunction()

# A patch rebuild replaces the prepared files after Ninja has inspected their
# old timestamps. Objects must depend on preparation itself, not only its order.
function(knurl_kore_object_dependencies stamp)
  foreach(source IN LISTS ARGN)
    set_property(SOURCE "${source}" APPEND PROPERTY OBJECT_DEPENDS "${stamp}")
  endforeach()
endfunction()
