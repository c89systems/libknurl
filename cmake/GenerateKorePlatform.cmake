file(MAKE_DIRECTORY "${OUTPUT_DIRECTORY}")
execute_process(COMMAND "${CMAKE_COMMAND}" -E env "TARGET_PLATFORM=${TARGET_PLATFORM}"
  "${KORE_SOURCE}/misc/linux-platform.sh"
  WORKING_DIRECTORY "${KORE_SOURCE}"
  RESULT_VARIABLE result OUTPUT_FILE "${OUTPUT_DIRECTORY}/platform.h")
if(NOT result EQUAL 0)
  message(FATAL_ERROR "Unable to generate the Kore platform header")
endif()
