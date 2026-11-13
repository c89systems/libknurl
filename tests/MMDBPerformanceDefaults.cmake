include("${CMAKE_CURRENT_LIST_DIR}/MMDBPerformance.cmake")
file(READ "${OUTPUT_DIR}/comparison.txt" results)
if(NOT results MATCHES "heap=524288 " OR
    results MATCHES "heap=(8192|65536|262144) ")
  message(FATAL_ERROR "Explicit zero must measure the 512 KiB default: ${results}")
endif()
