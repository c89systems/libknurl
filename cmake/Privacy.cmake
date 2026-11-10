cmake_minimum_required(VERSION 3.24)
include_guard(GLOBAL)

# Inspect extracted, declared payload files. Archive layout/identity belongs to
# the product verifier; this helper neither extracts nor prints matched content.
function(knurl_verify_private_bytes path)
  find_program(KNURL_STRINGS strings REQUIRED)
  execute_process(COMMAND "${KNURL_STRINGS}" -a -n 1 "${path}"
    RESULT_VARIABLE code OUTPUT_VARIABLE bytes ERROR_VARIABLE errors)
  if(NOT code EQUAL 0)
    message(FATAL_ERROR "Cannot inspect artifact bytes: ${path}: ${errors}")
  endif()
  get_filename_component(owner "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." REALPATH)
  string(FIND "${bytes}" "${owner}/" checkout)
  if(NOT checkout EQUAL -1 OR
      bytes MATCHES "/(home|Users)/|(^|[ \t\r\n=\"'])/root/|/private/var/|fsanitize=|__asan_|__ubsan_|__afl_")
    message(FATAL_ERROR "Private path/instrumentation in artifact: ${path}")
  endif()
  # PEM labels also occur in ordinary API implementations. Require a complete
  # block and substantial payload, not a label or documentation placeholder.
  foreach(kind "PRIVATE KEY" "RSA PRIVATE KEY" "EC PRIVATE KEY" "DSA PRIVATE KEY"
      "OPENSSH PRIVATE KEY" "ENCRYPTED PRIVATE KEY")
    set(pattern "-----BEGIN ${kind}-----[\r\n]+(Proc-Type:[^\r\n]*[\r\n]+DEK-Info:[^\r\n]*[\r\n]+)?([A-Za-z0-9+/=\r\n]+)-----END ${kind}-----")
    string(REGEX MATCHALL "${pattern}" blocks "${bytes}")
    foreach(block IN LISTS blocks)
      string(REGEX MATCH "${pattern}" matched "${block}")
      string(REGEX REPLACE "[\r\n]" "" body "${CMAKE_MATCH_2}")
      string(LENGTH "${body}" length)
      if(length GREATER_EQUAL 64)
        message(FATAL_ERROR "Private key/credential in artifact: ${path}")
      endif()
    endforeach()
  endforeach()
  # Recognizable GitHub token forms. Short/redacted examples and variable names
  # are not credentials. This is a bounded leak check, not arbitrary secret discovery.
  string(REGEX MATCHALL "gh[pousr]_[A-Za-z0-9]+|github_pat_[A-Za-z0-9_]+" tokens "${bytes}")
  foreach(token IN LISTS tokens)
    string(LENGTH "${token}" length)
    if((token MATCHES "^github_pat_" AND length GREATER_EQUAL 60) OR
        (NOT token MATCHES "^github_pat_" AND length GREATER_EQUAL 40))
      message(FATAL_ERROR "Private key/credential in artifact: ${path}")
    endif()
  endforeach()
endfunction()

function(knurl_verify_archive_metadata path archiver)
  if(NOT archiver OR NOT EXISTS "${archiver}")
    message(FATAL_ERROR "Reconfigure the selected-target build to provide its SDK archiver")
  endif()
  execute_process(COMMAND "${CMAKE_COMMAND}" -E env LC_ALL=C TZ=UTC
    "${archiver}" tv "${path}" RESULT_VARIABLE code OUTPUT_VARIABLE records ERROR_VARIABLE errors)
  if(NOT code EQUAL 0 OR records STREQUAL "")
    message(FATAL_ERROR "Cannot inspect static archive metadata: ${path}: ${errors}")
  endif()
  string(STRIP "${records}" records)
  string(REPLACE "\n" ";" records "${records}")
  foreach(record IN LISTS records)
    if(NOT record MATCHES "^[^ ]+ +0/0 +[0-9]+ +Jan +1 +00:00 +1970 +")
      message(FATAL_ERROR "Private/nondeterministic static archive metadata: ${path}")
    endif()
  endforeach()
endfunction()

if(DEFINED PRIVACY_FILE)
  knurl_verify_private_bytes("${PRIVACY_FILE}")
endif()
