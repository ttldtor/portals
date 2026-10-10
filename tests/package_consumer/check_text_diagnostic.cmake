# A successful consumer build is the control for include/toolchain failures.
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target portals_compact_consumer
  RESULT_VARIABLE control OUTPUT_VARIABLE controlOutput ERROR_VARIABLE controlError)
if(NOT control EQUAL 0)
  message(FATAL_ERROR "Valid text consumer failed: ${controlOutput}\n${controlError}")
endif()
execute_process(
  COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target "${TARGET}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
set(diagnostic "${output}\n${error}")
if(result EQUAL 0)
  message(FATAL_ERROR "Invalid text format unexpectedly compiled")
endif()
if(NOT diagnostic MATCHES "constant expression|constexpr|consteval|format_string|_Checking_scanner")
  message(FATAL_ERROR "Compilation failed without a format-check diagnostic:\n${diagnostic}")
endif()
message(STATUS "Rejected invalid text format at compile time")
