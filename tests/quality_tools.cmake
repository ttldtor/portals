# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
cmake_minimum_required(VERSION 3.24)
foreach(required SOURCE_DIR TEST_ROOT_DIR)
  if(NOT DEFINED ${required})
    message(FATAL_ERROR "${required} is required")
  endif()
endforeach()
string(RANDOM LENGTH 16 ALPHABET 0123456789abcdef suffix)
set(root "${TEST_ROOT_DIR}/quality-${suffix}")
file(MAKE_DIRECTORY "${root}")
set(performance "${SOURCE_DIR}/tools/quality/performance.cmake")
set(readability "${SOURCE_DIR}/tools/quality/readability.cmake")

function(assertSuccessful result output)
  if(NOT result STREQUAL "0")
    message(FATAL_ERROR "expected successful quality command: ${result}: ${output}")
  endif()
endfunction()

function(assertFailed result output reason)
  if(result STREQUAL "0" OR NOT output MATCHES "${reason}")
    message(FATAL_ERROR "expected failure matching ${reason}: ${result}: ${output}")
  endif()
endfunction()

function(record name raw metadata kind expected)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DMODE=record "-DRAW_FILE=${raw}" "-DMETADATA_FILE=${metadata}"
    "-DOUTPUT_DIR=${root}/${name}" "-DKIND=${kind}" -P "${performance}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
  if(expected STREQUAL "PASS")
    assertSuccessful("${result}" "${output}${errors}")
    file(SHA256 "${raw}" originalHash)
    file(SHA256 "${root}/${name}/raw.json" preservedHash)
    if(NOT originalHash STREQUAL preservedHash)
      message(FATAL_ERROR "raw JSON bytes were not preserved")
    endif()
  else()
    assertFailed("${result}" "${output}${errors}" "FAIL")
  endif()
endfunction()

function(compare name before after expected reason)
  execute_process(COMMAND "${CMAKE_COMMAND}" -DMODE=compare "-DBEFORE_DIR=${root}/${before}"
    "-DAFTER_DIR=${root}/${after}" "-DOUTPUT_FILE=${root}/${name}.json" -P "${performance}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
  if(expected STREQUAL "PASS")
    assertSuccessful("${result}" "${output}${errors}")
  else()
    assertFailed("${result}" "${output}${errors}" "${reason}")
    if(EXISTS "${root}/${name}.json")
      message(FATAL_ERROR "failed comparison emitted a successful report")
    endif()
  endif()
endfunction()

set(metadata [=[{
  "revision":"fixture-before",
  "toolchain":{"compiler":"fixture compiler","configure_command":"fixture configure"},
  "configuration":"Release",
  "host":{"os":"fixture OS","cpu":"fixture CPU","power_mode":"fixture stable","background_load":"fixture none"},
  "command":["fixture-benchmark","--fixture"],"benchmark_exit_code":0,"workload":"fixture-only"
}]=])
file(WRITE "${root}/metadata.json" "${metadata}\n")
set(managed [=[{
  "schema":1,"complete":true,"correctness_verified":true,"check_only":false,
  "compiler":"fixture compiler","build":"Release","assertions_enabled":false,"pointer_bits":64,
  "clock":"steady_clock","unit":"ns","percentile":"nearest_rank","samples_per_case":3,"warmup_per_case":1,
  "cases":[{"api":"descriptions","rows":2,"scene_nodes":7,"styled_controls":4,
    "phases":{"construction":{"count":3,"p50":2,"p95":3,"min":1,"max":3,"samples":[1,2,3]}}}]
}]=])
string(REPLACE "\n" "\r\n" managedCrlf "${managed}")
file(WRITE "${root}/managed.json" "${managedCrlf}\r\n")
record(before "${root}/managed.json" "${root}/metadata.json" managed PASS)
record(after "${root}/managed.json" "${root}/metadata.json" managed PASS)
compare(equal before after PASS "")
file(READ "${root}/equal.json" equal)
string(JSON ratio GET "${equal}" metrics 1 after_over_before)
if(NOT ratio EQUAL 1)
  message(FATAL_ERROR "same observation must have ratio 1")
endif()

string(JSON zero SET "${managed}" cases 0 phases construction
  "{\"count\":3,\"p50\":0,\"p95\":0,\"min\":0,\"max\":0,\"samples\":[0,0,0]}")
file(WRITE "${root}/zero.json" "${zero}")
record(zero "${root}/zero.json" "${root}/metadata.json" managed PASS)
compare(zero-ratio zero after PASS "")
file(READ "${root}/zero-ratio.json" comparison)
string(JSON ratioType TYPE "${comparison}" metrics 1 after_over_before)
string(JSON observation GET "${comparison}" metrics 1 observation)
string(JSON count GET "${comparison}" metrics 1 after_evidence count)
if(NOT ratioType STREQUAL "NULL" OR NOT observation STREQUAL "CANDIDATE_REGRESSION" OR NOT count EQUAL 3)
  message(FATAL_ERROR "zero baseline or sample evidence is wrong")
endif()

string(JSON incompatibleMetadata SET "${metadata}" configuration "\"Debug\"")
file(WRITE "${root}/incompatible-metadata.json" "${incompatibleMetadata}")
record(contradictory-metadata "${root}/managed.json" "${root}/incompatible-metadata.json" managed FAIL)
string(JSON debugRaw SET "${managed}" build "\"Debug\"")
file(WRITE "${root}/debug.json" "${debugRaw}")
record(incompatible "${root}/debug.json" "${root}/incompatible-metadata.json" managed PASS)
compare(incompatible-config before incompatible FAIL "incompatible metadata")
string(JSON differentWork SET "${managed}" cases 0 rows 3)
file(WRITE "${root}/workload.json" "${differentWork}")
record(workload "${root}/workload.json" "${root}/metadata.json" managed PASS)
compare(incompatible-workload before workload FAIL "incompatible actual benchmark workload")

file(WRITE "${root}/malformed.json" "{ broken")
record(malformed "${root}/malformed.json" "${root}/metadata.json" managed FAIL)
string(JSON incorrectSummary SET "${managed}" cases 0 phases construction p50 7)
file(WRITE "${root}/incorrect-summary.json" "${incorrectSummary}")
record(incorrect-summary "${root}/incorrect-summary.json" "${root}/metadata.json" managed FAIL)
string(JSON incomplete SET "${managed}" complete false)
file(WRITE "${root}/incomplete.json" "${incomplete}")
record(incomplete "${root}/incomplete.json" "${root}/metadata.json" managed FAIL)
string(JSON invalidMetadata REMOVE "${metadata}" host)
file(WRITE "${root}/missing-metadata.json" "${invalidMetadata}")
record(missing-metadata "${root}/managed.json" "${root}/missing-metadata.json" managed FAIL)
string(JSON failedExit SET "${metadata}" benchmark_exit_code 2)
file(WRITE "${root}/failed-exit.json" "${failedExit}")
record(failed-exit "${root}/managed.json" "${root}/failed-exit.json" managed FAIL)
record(before "${root}/managed.json" "${root}/metadata.json" managed FAIL)

set(compact [=[{"samples":30,"warmup":3,"nodes":4,"cases":[{"api":"managed","phases":[{
  "phase":"mount_and_theme","p50_ns":10,"p95_ns":20,"allocation_calls_min":0,"allocation_calls_max":1}]}]}]=])
file(WRITE "${root}/compact.json" "${compact}")
record(compact-before "${root}/compact.json" "${root}/metadata.json" compact PASS)
record(compact-after "${root}/compact.json" "${root}/metadata.json" compact PASS)
compare(compact-comparison compact-before compact-after PASS "")
compare(incompatible-kind before compact-after FAIL "incompatible benchmark kinds")
file(READ "${root}/compact-comparison.json" compactComparison)
string(JSON rawAvailable GET "${compactComparison}" metrics 1 before_evidence raw_values_preserved)
if(rawAvailable)
  message(FATAL_ERROR "compact benchmark does not export raw timing arrays")
endif()

set(series [=[{"unit":"ns","count":3,"overflow":0,"percentiles_complete":true,"p50":2,"p95":3,"max":3,"raw":[1,2,3]}]=])
set(encoding "{\"schema\":1,\"kind\":\"separate_encoding\",\"complete\":true,\"oracle_verified\":true,\"capture\":\"fixture\",\"frames\":2,\"repetitions\":20,\"ascii\":true,\"ansi16\":false,\"full_bytes\":200,\"diff_bytes\":100,\"encode_full_ns\":${series},\"encode_diff_ns\":${series},\"render_saved_cell_scene_ns\":${series}}")
file(WRITE "${root}/encoding.json" "${encoding}")
record(encoding-before "${root}/encoding.json" "${root}/metadata.json" mvp PASS)
record(encoding-after "${root}/encoding.json" "${root}/metadata.json" mvp PASS)
compare(encoding-comparison encoding-before encoding-after PASS "")
set(emptySeries [=[{"unit":"ns","count":0,"overflow":0,"percentiles_complete":false,"p50":null,"p95":null,"max":null,"raw":[]}]=])
string(JSON empty SET "${encoding}" encode_full_ns "${emptySeries}")
file(WRITE "${root}/empty-series.json" "${empty}")
record(empty-series "${root}/empty-series.json" "${root}/metadata.json" mvp PASS)
compare(empty-series-comparison encoding-before empty-series PASS "")
file(READ "${root}/empty-series-comparison.json" emptyComparison)
string(JSON emptyStatus GET "${emptyComparison}" metrics 0 observation)
if(NOT emptyStatus STREQUAL "NOT_MEASURED")
  message(FATAL_ERROR "missing samples must stay NOT_MEASURED")
endif()

set(application "{\"schema\":1,\"kind\":\"application\",\"complete\":true,\"application_ok\":true,\"exit_code\":0,\"frame_cap_respected\":true,\"fixed_viewport_baseline\":true,\"runner_accounting_matches\":true,\"compiler\":\"fixture compiler\",\"build\":\"Release\",\"platform\":\"fixture OS\",\"transport\":\"headless\",\"generator\":\"fixture\",\"configured_rate\":100,\"seed\":42,\"rows\":1000,\"fps_cap\":30,\"warmup_ms\":5000,\"duration_ms\":30000,\"delay_ms\":0,\"ascii\":true,\"ansi16\":false,\"diagnostics_enabled\":false,\"sample_capacity\":65536,\"sample_boundary\":\"fixture\",\"acceptance\":{\"lossless_ohlc\":true,\"viewport\":{\"width\":120,\"height\":40},\"initial_viewport\":{\"width\":120,\"height\":40},\"resize_batches\":0},\"shutdown\":{\"provider_stopped\":true,\"terminal_closed\":true},\"phase_samples_ns\":{\"PREPARE\":${series},\"INPUT\":${emptySeries}},\"transport_read_ns\":${series},\"dashboard_display_ingress_ms\":${emptySeries},\"runner_input_control_ns\":${emptySeries},\"window\":{\"accepted_frames\":2,\"confirmed_bytes\":200,\"encoded_bytes\":200,\"wall_ns\":30000000000,\"process_cpu_ns\":20000000}}")
file(WRITE "${root}/application.json" "${application}")
record(application-before "${root}/application.json" "${root}/metadata.json" mvp PASS)
record(application-after "${root}/application.json" "${root}/metadata.json" mvp PASS)
compare(application-comparison application-before application-after PASS "")
string(JSON wrongSeed SET "${application}" seed 43)
file(WRITE "${root}/wrong-seed.json" "${wrongSeed}")
record(wrong-seed "${root}/wrong-seed.json" "${root}/metadata.json" mvp PASS)
compare(incompatible-seed application-before wrong-seed FAIL "incompatible actual benchmark workload")

string(JSON overflow SET "${series}" overflow 1)
string(JSON overflow SET "${overflow}" percentiles_complete false)
foreach(stat p50 p95 max)
  string(JSON overflow SET "${overflow}" "${stat}" null)
endforeach()
string(JSON overflowRun SET "${encoding}" encode_full_ns "${overflow}")
file(WRITE "${root}/overflow-series.json" "${overflowRun}")
record(overflow-series "${root}/overflow-series.json" "${root}/metadata.json" mvp PASS)
compare(overflow-comparison encoding-before overflow-series PASS "")
file(READ "${root}/overflow-comparison.json" overflowComparison)
string(JSON overflowStatus GET "${overflowComparison}" metrics 0 observation)
string(JSON retainedMinimum GET "${overflowComparison}" metrics 0 after_evidence retained_min)
if(NOT overflowStatus STREQUAL "NOT_MEASURED" OR NOT retainedMinimum EQUAL 1)
  message(FATAL_ERROR "overflow must preserve retained range without complete percentiles")
endif()

file(APPEND "${root}/after/raw.json" " ")
compare(tampered-raw before after FAIL "SHA256 mismatch")

file(MAKE_DIRECTORY "${root}/compile-db")
file(WRITE "${root}/source.cpp" "int f(int x) { if (x) { return 1; } return 0; }\n")
file(WRITE "${root}/compile-db/compile_commands.json"
  "[{\"directory\":\"${root}\",\"file\":\"${root}/source.cpp\",\"command\":\"clang++ -std=c++23 -c source.cpp\"}]")
execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${root}/compile-db" "-DFILES=${root}/source.cpp"
  "-DOUTPUT_DIR=${root}/unavailable-clang" "-DCLANG_TIDY=${root}/missing-clang-tidy" -P "${readability}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
assertFailed("${result}" "${output}${errors}" "NOT_RUN")
file(READ "${root}/unavailable-clang/status.json" status)
string(JSON state GET "${status}" status)
if(NOT state STREQUAL "NOT_RUN")
  message(FATAL_ERROR "unavailable clang must not claim zero issues")
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${root}/compile-db" "-DFILES=${root}/source.cpp"
  "-DOUTPUT_DIR=${root}/unsupported-driver" "-DCLANG_TIDY=${CMAKE_COMMAND}" -P "${readability}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
assertFailed("${result}" "${output}${errors}" "NOT_RUN")
if(NOT EXISTS "${root}/unsupported-driver/version.txt")
  message(FATAL_ERROR "tool version evidence must survive unsupported-driver failure")
endif()

# A fixture provider tests diagnostic parsing without claiming real AST measurements.
if(CMAKE_HOST_WIN32)
  set(fixtureProvider "${root}/fixture-clang.cmd")
  file(WRITE "${fixtureProvider}" "@echo off\necho fixture provider only\necho readability-function-cognitive-complexity\necho source.cpp:1:5: warning: function 'fixture' has cognitive complexity of 1 (threshold 0) [readability-function-cognitive-complexity]\necho     1 ^| // error: context text\nexit /b 0\n")
else()
  set(fixtureProvider "${root}/fixture-clang.sh")
  file(WRITE "${fixtureProvider}" "#!/bin/sh\ncat <<'QUALITY_FIXTURE'\nfixture provider only\nreadability-function-cognitive-complexity\nsource.cpp:1:5: warning: function 'fixture' has cognitive complexity of 1 (threshold 0) [readability-function-cognitive-complexity]\n    1 | // error: context text\nQUALITY_FIXTURE\n")
  file(CHMOD "${fixtureProvider}" PERMISSIONS OWNER_READ OWNER_WRITE OWNER_EXECUTE)
endif()
execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${root}/compile-db" "-DFILES=${root}/source.cpp"
  "-DOUTPUT_DIR=${root}/fixture-source-error-text" "-DCLANG_TIDY=${fixtureProvider}"
  "-DEXTRA_ARGS=-DQUALITY_FIXTURE=1" -P "${readability}"
  RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 10)
assertSuccessful("${result}" "${output}${errors}")
file(READ "${root}/fixture-source-error-text/status.json" fixtureStatus)
string(JSON fixtureState GET "${fixtureStatus}" status)
string(JSON fixtureArgument GET "${fixtureStatus}" compiler_extra_args 0)
if(NOT fixtureState STREQUAL "REPORTED" OR NOT fixtureArgument STREQUAL "-DQUALITY_FIXTURE=1")
  message(FATAL_ERROR "source error text or compiler extra-argument provenance was misinterpreted")
endif()
if(DEFINED TEST_CLANG_TIDY)
  file(MAKE_DIRECTORY "${root}/header-live/sources" "${root}/header-live/headers" "${root}/header-live/build")
  file(WRITE "${root}/header-live/headers/shared.hpp"
    "inline int headerFunction(int x) { if (x) { return 2; } return 0; }\n")
  foreach(tu a b)
    file(WRITE "${root}/header-live/sources/${tu}.cpp"
      "#include \"../headers/shared.hpp\"\nint classify(int x) { if (x > 1) { return headerFunction(x); } return 0; } // error: context text\n")
  endforeach()
  file(WRITE "${root}/header-live/build/compile_commands.json"
    "[{\"directory\":\"${root}/header-live\",\"file\":\"${root}/header-live/sources/a.cpp\",\"command\":\"clang++ -std=c++23 -c sources/a.cpp\"},{\"directory\":\"${root}/header-live\",\"file\":\"${root}/header-live/sources/b.cpp\",\"command\":\"clang++ -std=c++23 -c sources/b.cpp\"}]")
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${root}/header-live/build"
    "-DFILES=${root}/header-live/sources/a.cpp;${root}/header-live/sources/b.cpp"
    "-DHEADERS=${root}/header-live/headers/shared.hpp"
    "-DOUTPUT_DIR=${root}/header-live/receipt" "-DCLANG_TIDY=${TEST_CLANG_TIDY}" -P "${readability}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
  assertSuccessful("${result}" "${output}${errors}")
  file(READ "${root}/header-live/receipt/status.json" live)
  string(JSON liveCount GET "${live}" positive_complexity_diagnostics)
  string(JSON liveHeaders GET "${live}" explicit_headers)
  if(NOT liveCount EQUAL 3 OR NOT liveHeaders EQUAL 1)
    message(FATAL_ERROR "header must be retained once while same-named functions in distinct TUs remain distinct")
  endif()
  file(WRITE "${root}/header-live/headers/shared.hpp" "inline int broken( {\n")
  execute_process(COMMAND "${CMAKE_COMMAND}" "-DBUILD_DIR=${root}/header-live/build"
    "-DFILES=${root}/header-live/sources/a.cpp" "-DHEADERS=${root}/header-live/headers/shared.hpp"
    "-DOUTPUT_DIR=${root}/header-live/compiler-failure" "-DCLANG_TIDY=${TEST_CLANG_TIDY}" -P "${readability}"
    RESULT_VARIABLE result OUTPUT_VARIABLE output ERROR_VARIABLE errors TIMEOUT 30)
  assertFailed("${result}" "${output}${errors}" "FAIL")
  file(READ "${root}/header-live/compiler-failure/status.json" failure)
  string(JSON failureStatus GET "${failure}" status)
  if(NOT failureStatus STREQUAL "FAIL")
    message(FATAL_ERROR "compiler errors must not be reported as zero issues")
  endif()
  message(STATUS "Live Clang-tidy TU/header/dedup/compiler-failure integration PASS")
else()
  message(STATUS "Live Clang-tidy integration NOT_RUN: supply TEST_CLANG_TIDY to enable it")
endif()
message(STATUS "Quality fixture checks PASS; receipts retained in ${root}")
