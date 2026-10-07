cmake_minimum_required(VERSION 3.24)
project(Portals VERSION 0.1.0 LANGUAGES CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Use an out-of-tree build directory, for example cmake -S . -B build/debug")
endif()

option(PORTALS_BUILD_TESTS "Build Portals headless tests" ${PROJECT_IS_TOP_LEVEL})
option(PORTALS_BUILD_EXAMPLES "Build Portals example consumer" ${PROJECT_IS_TOP_LEVEL})
option(PORTALS_BUILD_DOCS "Build Portals API documentation with Doxygen" OFF)

add_library(portals_core STATIC src/version.cpp src/geometry.cpp)
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
  add_executable(portals_geometry tests/geometry.cpp)
  target_link_libraries(portals_geometry PRIVATE portals::core)
  portals_enable_warnings(portals_geometry)
  add_test(NAME portals.geometry COMMAND portals_geometry)
  set_tests_properties(portals.smoke portals.geometry PROPERTIES TIMEOUT 10)
endif()

if(PORTALS_BUILD_DOCS)
  find_package(Doxygen REQUIRED)
  set(DOXYGEN_GENERATE_HTML YES)
  set(DOXYGEN_GENERATE_LATEX NO)
  set(DOXYGEN_HAVE_DOT NO)
  set(DOXYGEN_WARN_AS_ERROR YES)
  set(DOXYGEN_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/docs")
  doxygen_add_docs(portals_docs include README.md docs/geometry.md
    docs/api-design.md experiments/module-spike/README.md
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    COMMENT "Generate Portals API documentation"
  )
endif()
