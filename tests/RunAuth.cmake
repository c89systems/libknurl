include("${CMAKE_CURRENT_LIST_DIR}/../cmake/OwnedPaths.cmake")
knurl_require_owned_path("${WORK}" "Auth fixture")
file(REMOVE_RECURSE "${WORK}")
file(MAKE_DIRECTORY "${WORK}")
find_program(openssl openssl REQUIRED)
execute_process(COMMAND "${openssl}" req -x509 -newkey rsa:2048 -nodes
  -subj /CN=localhost -addext subjectAltName=DNS:localhost -days 2
  -keyout "${WORK}/key.pem" -out "${WORK}/cert.pem"
  OUTPUT_QUIET ERROR_VARIABLE diagnostic RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "Unable to generate the auth fixture certificate\n${diagnostic}")
endif()
if(DEFINED RUNNER)
  execute_process(COMMAND "${TEST}" "${WORK}" "${RUNNER}" "${SCRIPT}" COMMAND_ERROR_IS_FATAL ANY)
else()
  execute_process(COMMAND "${TEST}" "${WORK}" COMMAND_ERROR_IS_FATAL ANY)
endif()
