# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
set(logDirectory "${BUILD_DIR}/pss-diagnostics/${CONFIG}")
file(MAKE_DIRECTORY "${logDirectory}")

# A missing header or broken implementation cannot count as a successful rejection.
execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target "${CONTROL}"
  RESULT_VARIABLE control OUTPUT_VARIABLE controlOutput ERROR_VARIABLE controlError)
file(WRITE "${logDirectory}/${TARGET}-control.log" "${controlOutput}\n${controlError}\nExit code: ${control}\n")
if(NOT control EQUAL 0)
  message(FATAL_ERROR "Valid PSS consumer failed; see ${logDirectory}/${TARGET}-control.log")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${BUILD_DIR}" --config "${CONFIG}" --target "${TARGET}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
set(diagnostic "${output}\n${error}")
file(WRITE "${logDirectory}/${TARGET}.log" "${diagnostic}\nExit code: ${result}\nExpected: ${EXPECTED}\n")
if(result EQUAL 0)
  message(FATAL_ERROR "Invalid PSS unexpectedly compiled; see ${logDirectory}/${TARGET}.log")
endif()
if(NOT diagnostic MATCHES "${EXPECTED}")
  message(FATAL_ERROR "Compilation failed without required category (${EXPECTED}); see ${logDirectory}/${TARGET}.log")
endif()

# Keep portable size evidence without assuming identical vendor wording or line layout.
string(LENGTH "${diagnostic}" diagnosticBytes)
string(REGEX REPLACE "[^\n]" "" newlines "${diagnostic}")
string(LENGTH "${newlines}" newlineCount)
math(EXPR diagnosticLines "${newlineCount} + 1")
file(WRITE "${logDirectory}/${TARGET}-size.txt"
  "bytes=${diagnosticBytes}\nlines=${diagnosticLines}\nexit=${result}\nexpected=${EXPECTED}\n")
message(STATUS "Rejected PSS at ${EXPECTED}; ${diagnosticBytes} bytes, ${diagnosticLines} lines; ${logDirectory}/${TARGET}.log")
