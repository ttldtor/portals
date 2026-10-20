# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
# Included by the documentation build. Development labels require a Git checkout;
# release and source-archive builds use the project version without Git.
option(PORTALS_DOCS_DEVELOPMENT "Label documentation as a development build with its Git revision and UTC build date" OFF)
if(NOT DEFINED DOXYGEN_PROJECT_NUMBER)
  set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION}")
endif()
if(PORTALS_DOCS_DEVELOPMENT)
  find_package(Git REQUIRED)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
    RESULT_VARIABLE documentation_git_result
    OUTPUT_VARIABLE documentation_commit OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_VARIABLE documentation_git_error)
  if(NOT documentation_git_result EQUAL 0 OR NOT documentation_commit MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Development documentation requires a Git commit: ${documentation_git_error}")
  endif()
  string(SUBSTRING "${documentation_commit}" 0 12 documentation_short_commit)
  string(TIMESTAMP documentation_build_date "%Y-%m-%d" UTC)
  set(DOXYGEN_PROJECT_NUMBER "${PROJECT_VERSION} · dev (${documentation_short_commit} · ${documentation_build_date} UTC)")
endif()
