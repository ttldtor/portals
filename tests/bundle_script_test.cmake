# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
# Lightweight contract tests: no compiler, terminal or repository mutation.
cmake_minimum_required(VERSION 3.24)
if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "Bundle script tests require SOURCE_DIR and BINARY_DIR")
endif()
include("${SOURCE_DIR}/tools/package/common.cmake")
string(RANDOM LENGTH 12 ALPHABET 0123456789abcdef runId)
set(testRoot "${BINARY_DIR}/bundle-script-${runId}")
bundleRequireNewPath(testRoot)
file(MAKE_DIRECTORY "${testRoot}/пакет с пробелами")
set(payload "${testRoot}/пакет с пробелами")
file(WRITE "${payload}/sample.txt" "committed payload\n")
bundleWriteHashes("${payload}" hashes)
file(WRITE "${payload}/manifest.json" "{\"schema_version\":1,\"files\":${hashes}}\n")
set(smoke "${SOURCE_DIR}/tests/bundle_smoke.cmake")

function(expectResult expected pattern)
  execute_process(COMMAND ${ARGN} RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE error)
  set(evidence "${output}\n${error}")
  if(expected STREQUAL "pass")
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Expected success; got ${result}: ${evidence}")
    endif()
  elseif(result EQUAL 0 OR NOT evidence MATCHES "${pattern}")
    message(FATAL_ERROR "Expected rejection matching ${pattern}; got ${result}: ${evidence}")
  endif()
endfunction()

expectResult(pass "" "${CMAKE_COMMAND}" "-DBUNDLE_DIR=${payload}" -DVERIFY_ONLY=ON -P "${smoke}")
file(APPEND "${payload}/sample.txt" "tampered\n")
expectResult(fail "SHA256 mismatch" "${CMAKE_COMMAND}" "-DBUNDLE_DIR=${payload}" -DVERIFY_ONLY=ON -P "${smoke}")
file(WRITE "${payload}/sample.txt" "committed payload\n")
file(WRITE "${payload}/unexpected.txt" "unlisted\n")
expectResult(fail "inventory differs" "${CMAKE_COMMAND}" "-DBUNDLE_DIR=${payload}" -DVERIFY_ONLY=ON -P "${smoke}")
# Only the test's own just-created extra file is removed, never a directory.
file(REMOVE "${payload}/unexpected.txt")
file(WRITE "${payload}/manifest.json" "{\"schema_version\":1,\"files\":{\"../outside.txt\":\"bad\"}}")
expectResult(fail "Unsafe bundle-relative path" "${CMAKE_COMMAND}" "-DBUNDLE_DIR=${payload}" -DVERIFY_ONLY=ON -P "${smoke}")
# Round-trip a real archive, including a Unicode directory and foreign work path.
file(WRITE "${payload}/manifest.json" "{\"schema_version\":1,\"files\":${hashes}}\n")
set(archive "${testRoot}/fixture.zip")
get_filename_component(payloadName "${payload}" NAME)
execute_process(COMMAND "${CMAKE_COMMAND}" -E tar cf "${archive}" --format=zip "${payloadName}"
  WORKING_DIRECTORY "${testRoot}" RESULT_VARIABLE archiveResult)
if(NOT archiveResult EQUAL 0)
  message(FATAL_ERROR "Cannot create bundle hash fixture archive")
endif()
expectResult(pass "" "${CMAKE_COMMAND}" "-DARCHIVE=${archive}"
  "-DTEST_WORK_DIR=${testRoot}/fresh unpack проверка" -DVERIFY_ONLY=ON -P "${smoke}")
expectResult(fail "TEST_WORK_DIR already exists" "${CMAKE_COMMAND}" "-DARCHIVE=${archive}"
  "-DTEST_WORK_DIR=${testRoot}/fresh unpack проверка" -DVERIFY_ONLY=ON -P "${smoke}")
set(bundleScript "${SOURCE_DIR}/tools/package/bundle.cmake")
expectResult(fail "WORK_DIR already exists" "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SOURCE_DIR}"
  "-DWORK_DIR=${testRoot}" "-DOUTPUT_ARCHIVE=${testRoot}-new.zip" -P "${bundleScript}")
expectResult(fail "WORK_DIR must be outside SOURCE_DIR" "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SOURCE_DIR}"
  "-DWORK_DIR=${SOURCE_DIR}/bundle-rejected-${runId}" "-DOUTPUT_ARCHIVE=${testRoot}/nested.zip" -P "${bundleScript}")
file(WRITE "${testRoot}/keep.zip" "unrelated user artifact")
expectResult(fail "OUTPUT_ARCHIVE already exists" "${CMAKE_COMMAND}" "-DSOURCE_DIR=${SOURCE_DIR}"
  "-DWORK_DIR=${testRoot}-new" "-DOUTPUT_ARCHIVE=${testRoot}/keep.zip" -P "${bundleScript}")
file(READ "${testRoot}/keep.zip" preserved)
if(NOT preserved STREQUAL "unrelated user artifact" OR EXISTS "${testRoot}-new")
  message(FATAL_ERROR "Rejected paths changed unrelated state")
endif()
message(STATUS "Bundle script contracts passed; evidence retained: ${testRoot}")
