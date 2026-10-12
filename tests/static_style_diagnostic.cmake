# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target "${CONTROL}"
  RESULT_VARIABLE control OUTPUT_VARIABLE controlOutput ERROR_VARIABLE controlError)
set(logDirectory "${BUILD_DIR}/static-style-diagnostics/${CONFIG}")
file(MAKE_DIRECTORY "${logDirectory}")
file(WRITE "${logDirectory}/${TARGET}-control.log" "${controlOutput}\n${controlError}\nExit code: ${control}\n")
if(NOT control EQUAL 0)
  message(FATAL_ERROR "Valid static style consumer failed; see ${logDirectory}/${TARGET}-control.log")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target "${TARGET}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
set(diagnostic "${output}\n${error}")
file(WRITE "${logDirectory}/${TARGET}.log" "${diagnostic}\nExit code: ${result}\nExpected: ${EXPECTED}\n")
if(result EQUAL 0)
  message(FATAL_ERROR "Invalid static style unexpectedly compiled; see ${logDirectory}/${TARGET}.log")
endif()
if(NOT diagnostic MATCHES "${EXPECTED}")
  message(FATAL_ERROR "Compilation failed without the required diagnostic (${EXPECTED}); see ${logDirectory}/${TARGET}.log")
endif()
message(STATUS "Rejected static style at ${EXPECTED}; full evidence: ${logDirectory}/${TARGET}.log")
