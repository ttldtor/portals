cmake_minimum_required(VERSION 3.24)
project(Portals VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-tree build directory, for example cmake -S . -B build/debug")
endif()

option(PORTALS_BUILD_TESTS "Build Portals headless tests" ${PROJECT_IS_TOP_LEVEL})
option(PORTALS_BUILD_EXAMPLES "Build Portals example consumer" ${PROJECT_IS_TOP_LEVEL})
option(PORTALS_BUILD_DOCS "Build Portals API documentation with Doxygen" OFF)
option(PORTALS_BUILD_TOOLS "Build the standalone terminal diagnostic probe" ${PROJECT_IS_TOP_LEVEL})

add_library(portals_core STATIC src/version.cpp)
add_library(portals::core ALIAS portals_core)
target_compile_features(portals_core PUBLIC cxx_std_23)
set_target_properties(portals_core PROPERTIES CXX_EXTENSIONS OFF)
target_include_directories(portals_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(portals_core PRIVATE
  PORTALS_VERSION_MAJOR=${PROJECT_VERSION_MAJOR}
  PORTALS_VERSION_MINOR=${PROJECT_VERSION_MINOR}
  PORTALS_VERSION_PATCH=${PROJECT_VERSION_PATCH}
)

function(portals_enable_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /permissive-)
  elseif(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

portals_enable_warnings(portals_core)

if(PORTALS_BUILD_TOOLS)
  add_executable(portals_terminal_probe tools/terminal_probe/main.cpp tools/terminal_probe/query.cpp)
  if(WIN32)
    target_sources(portals_terminal_probe PRIVATE tools/terminal_probe/native_windows.cpp)
  elseif(UNIX)
    target_sources(portals_terminal_probe PRIVATE tools/terminal_probe/native_posix.cpp)
  else()
    message(FATAL_ERROR "The terminal probe requires Windows or a POSIX platform; disable PORTALS_BUILD_TOOLS")
  endif()
  target_compile_features(portals_terminal_probe PRIVATE cxx_std_23)
  set_target_properties(portals_terminal_probe PROPERTIES CXX_EXTENSIONS OFF)
  if(MSVC)
    # The single-threaded diagnostic only reads getenv results; keep this CRT
    # portability choice private to the tool rather than changing core warnings.
    target_compile_definitions(portals_terminal_probe PRIVATE _CRT_SECURE_NO_WARNINGS)
  endif()
  portals_enable_warnings(portals_terminal_probe)
endif()

if(PORTALS_BUILD_EXAMPLES)
  add_subdirectory(examples/consumer)
  portals_enable_warnings(portals_consumer)
endif()

if(PORTALS_BUILD_TESTS)
  enable_testing()
  add_executable(portals_smoke tests/smoke.cpp)
  target_link_libraries(portals_smoke PRIVATE portals::core)
  portals_enable_warnings(portals_smoke)
  add_test(NAME portals.smoke COMMAND portals_smoke)
  set_tests_properties(portals.smoke PROPERTIES TIMEOUT 10)

  add_executable(portals_probe_query_tests tests/probe_query.cpp tools/terminal_probe/query.cpp)
  target_include_directories(portals_probe_query_tests PRIVATE tools/terminal_probe)
  target_compile_features(portals_probe_query_tests PRIVATE cxx_std_23)
  set_target_properties(portals_probe_query_tests PROPERTIES CXX_EXTENSIONS OFF)
  portals_enable_warnings(portals_probe_query_tests)
  add_test(NAME portals.probe.query COMMAND portals_probe_query_tests)
  set_tests_properties(portals.probe.query PROPERTIES TIMEOUT 10)

  if(PORTALS_BUILD_TOOLS)
    add_test(NAME portals.probe.redirected
      COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:portals_terminal_probe>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/probe_redirected.cmake)
    set_tests_properties(portals.probe.redirected PROPERTIES TIMEOUT 10)

    if(UNIX)
      add_executable(portals_probe_posix_tests tests/probe_posix.cpp)
      target_compile_features(portals_probe_posix_tests PRIVATE cxx_std_23)
      set_target_properties(portals_probe_posix_tests PROPERTIES CXX_EXTENSIONS OFF)
      portals_enable_warnings(portals_probe_posix_tests)
      add_test(NAME portals.probe.posix-fixture
        COMMAND portals_probe_posix_tests $<TARGET_FILE:portals_terminal_probe>)
      set_tests_properties(portals.probe.posix-fixture PROPERTIES TIMEOUT 15)
    endif()
  endif()
endif()

if(PORTALS_BUILD_DOCS)
  find_package(Doxygen REQUIRED)
  set(DOXYGEN_GENERATE_HTML YES)
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_HAVE_DOT NO)
  set(DOXYGEN_WARN_AS_ERROR YES)
  set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
  doxygen_add_docs(portals_docs include README.md docs/terminal-probe.md
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generate Portals API documentation"
  )
endif()
