file(MAKE_DIRECTORY "${WORK}")
execute_process(COMMAND "${HOST}" "${BINARY}" "${SCRIPT}" "${WORK}" "${MODE}"
  RESULT_VARIABLE status)
if(NOT status EQUAL 0)
  message(FATAL_ERROR "knurl application lifecycle failed (${MODE}, ${status})")
endif()
