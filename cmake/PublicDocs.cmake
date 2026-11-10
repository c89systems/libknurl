# Paths relative to the checkout and to each package's documentation root.
set(knurl_public_docs
  README.md
  docs/README.md
  docs/acme.md
  docs/assets.md
  docs/channels.md
  docs/editors.md
  docs/json.md
  docs/logging.md
  docs/lua.md
  docs/openapi.md
  docs/overview.md
  docs/policy.md
  docs/processes.md
  docs/proxy-core.md
  docs/proxy-http.md
  docs/proxy-websocket.md
  docs/requests.md
  docs/responses.md
  docs/rest.md
  docs/sdk.md
  docs/server-api-spec.md
  docs/storage.md
  docs/tls.md
  docs/uploads.md
  docs/webdav.md
  docs/websockets.md
  docs/xml.md
  lua/README.md
)

function(knurl_verify_public_docs source_root packaged_root)
  foreach(path IN LISTS knurl_public_docs)
    if(NOT EXISTS "${packaged_root}/${path}")
      message(FATAL_ERROR "Missing public documentation: ${path}")
    endif()
    file(SHA256 "${source_root}/${path}" source_digest)
    file(SHA256 "${packaged_root}/${path}" packaged_digest)
    if(NOT packaged_digest STREQUAL source_digest)
      message(FATAL_ERROR "Packaged documentation differs from source: ${path}")
    endif()
    # The guides use inline Markdown links with plain relative paths.
    file(READ "${packaged_root}/${path}" markdown)
    string(REGEX MATCHALL "\\[[^]\r\n]+\\]\\([^() \t\r\n]+\\)" links "${markdown}")
    get_filename_component(directory "${packaged_root}/${path}" DIRECTORY)
    foreach(link IN LISTS links)
      string(REGEX REPLACE "^\\[[^]]+\\]\\(|\\)$" "" target "${link}")
      if(target MATCHES "^([a-zA-Z][a-zA-Z0-9+.-]*:|#)")
        continue()
      endif()
      string(REGEX REPLACE "#.*$" "" target "${target}")
      cmake_path(ABSOLUTE_PATH target BASE_DIRECTORY "${directory}" NORMALIZE)
      cmake_path(IS_PREFIX packaged_root "${target}" NORMALIZE inside)
      if(NOT inside OR NOT EXISTS "${target}")
        message(FATAL_ERROR "Broken packaged documentation link in ${path}: ${link}")
      endif()
    endforeach()
  endforeach()
endfunction()
