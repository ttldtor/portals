# Copyright (c) 2026 ttldtor.
# SPDX-License-Identifier: BSL-1.0
# Internal portals_docs finalizer. Required HTML_DIR: existing generated HTML
# directory. Rewrites known links/index defects and copies linked notices/sources
# in place. Invalid inputs or I/O errors fail the CMake script; no rollback.
# Finalize portable example links and recognized Doxygen 1.18 index defects.
if(NOT DEFINED HTML_DIR OR NOT IS_DIRECTORY "${HTML_DIR}")
  message(FATAL_ERROR "HTML_DIR must name the generated Doxygen HTML directory")
endif()

include("${CMAKE_CURRENT_LIST_DIR}/FinalizeDiagrams.cmake")

foreach(notice IN ITEMS vendor/pfr/LICENSE_1_0.txt vendor/doctest/LICENSE.txt vendor/lyra/LICENSE.txt vendor/lyra/manifest.json tests/fixtures/unicode/manifest.json)
  get_filename_component(notice_directory "${notice}" DIRECTORY)
  file(MAKE_DIRECTORY "${HTML_DIR}/${notice_directory}")
  configure_file("${CMAKE_CURRENT_LIST_DIR}/../${notice}" "${HTML_DIR}/${notice}" COPYONLY)
endforeach()

# GitHub links are relative to docs/*.md; generated HTML has a flat layout.
# Ship the exact referenced sources inside the HTML tree so an offline copy
# works without the repository or a hard-coded GitHub branch URL.
set(example_sources
  docs/examples/tutorial_note_1.cpp
  docs/examples/tutorial_note_2.cpp
  docs/examples/tutorial_note_3.cpp
  docs/examples/tutorial_note_4.cpp
  docs/examples/docs_basic.cpp
  tests/package_consumer/named_scoped.cpp
  docs/examples/docs_events.cpp
  docs/examples/docs_state.cpp
  tests/package_consumer/pipe_contracts.cpp
  tests/package_consumer/pipe_contracts_methods.cpp
  tests/support/gallery_before.hpp
  tests/support/examples/custom_widget/meter.hpp
  tests/package_consumer/form_declarative.cpp
  examples/gallery/main.cpp
  examples/custom_widget/main.cpp
  examples/editor_history/main.cpp
  examples/editor_history/editor.hpp
  examples/dashboard/main.cpp
  examples/click_handlers/main.cpp
  examples/gallery/fluent_form.hpp
  examples/gallery/options.hpp
  examples/gallery/settings.hpp
  examples/gallery/themes.hpp
  examples/custom_widget/meter.hpp
  examples/custom_widget/fluent_meter.hpp
  examples/dashboard/dashboard.hpp
  examples/dashboard/provider.hpp
  tests/support/examples/gallery/form.hpp
  tests/support/examples/replay/terminal.hpp
  tests/compactness.cpp
  tests/package_consumer/async_callback.cpp
  tests/package_consumer/async_coroutine.cpp
  tests/package_consumer/async_scenario.hpp
  tests/package_consumer/composed_named.cpp
  tests/package_consumer/composed_support.hpp
  tests/package_consumer/ct_pss.cpp
  tests/package_consumer/ct_pss_metadata.cpp
  tests/package_consumer/form_replay.hpp
  tests/package_consumer/form_support.hpp
  tests/package_consumer/shortcut_sequences.cpp
  tests/package_consumer/static_scene.cpp
  tests/package_consumer/static_scene_support.hpp
  tests/package_consumer/static_styles.cpp
  tests/package_consumer/static_styles_support.hpp
  tests/package_consumer/text_property.cpp
  tests/package_consumer/virtual_lifecycle.cpp
  tests/package_consumer/virtual_support.hpp
  tools/compactness/compactness.hpp)
foreach(source IN LISTS example_sources)
  get_filename_component(directory "${source}" DIRECTORY)
  file(MAKE_DIRECTORY "${HTML_DIR}/source/${directory}")
  configure_file("${CMAKE_CURRENT_LIST_DIR}/../${source}" "${HTML_DIR}/source/${source}" COPYONLY)
endforeach()

file(GLOB guide_html_files "${HTML_DIR}/*.html")
foreach(page_file IN LISTS guide_html_files)
  file(READ "${page_file}" content)
  foreach(notice IN ITEMS LICENSE.txt vendor/pfr/LICENSE_1_0.txt vendor/doctest/LICENSE.txt
      vendor/textual/LICENSE vendor/lyra/LICENSE.txt vendor/lyra/manifest.json tests/fixtures/unicode/Unicode-LICENSE.txt tests/fixtures/unicode/manifest.json)
    string(REPLACE "href=\"../../${notice}\"" "href=\"${notice}\"" content "${content}")
    string(REPLACE "href=\"../${notice}\"" "href=\"${notice}\"" content "${content}")
  endforeach()
  foreach(source IN LISTS example_sources)
    string(REPLACE "href=\"../${source}\"" "href=\"source/${source}\"" content "${content}")
    string(REPLACE "href=\"../../${source}\"" "href=\"source/${source}\"" content "${content}")
  endforeach()
  foreach(doc_example IN ITEMS docs_basic.cpp docs_events.cpp docs_state.cpp
      tutorial_note_1.cpp tutorial_note_2.cpp tutorial_note_3.cpp tutorial_note_4.cpp)
    string(REPLACE "href=\"examples/${doc_example}\"" "href=\"source/docs/examples/${doc_example}\"" content "${content}")
    string(REPLACE "href=\"../examples/${doc_example}\"" "href=\"source/docs/examples/${doc_example}\"" content "${content}")
  endforeach()
  # Keep the Markdown deep link usable on GitHub and in the generated HTML.
  string(REPLACE "href=\"declarative-ui.md#pipe-click-handlers\""
    "href=\"declarative-ui.html#pipe-click-handlers\"" content "${content}")
  string(REPLACE "href=\"declarative-ui.md#short-key-bindings\""
    "href=\"declarative-ui.html#short-key-bindings\"" content "${content}")
  string(REPLACE "href=\"styling-guide.md#future-style-parameters\""
    "href=\"styling-guide.html#future-style-parameters\"" content "${content}")
  string(REPLACE "href=\"styling-guide.md#future-style-parameters-ru\""
    "href=\"styling-guide-ru.html#future-style-parameters-ru\"" content "${content}")
  # BEGIN GENERATED FRAGMENT TARGETS
  string(REPLACE "href=\"declarative-ui.md#pipe-click-handlers\"" "href=\"declarative-ui.html#pipe-click-handlers\"" content "${content}")
  string(REPLACE "href=\"declarative-ui.md#short-key-bindings\"" "href=\"declarative-ui.html#short-key-bindings\"" content "${content}")
  string(REPLACE "href=\"declarative-ui.md#typed-columns-and-named-view-settings\"" "href=\"declarative-ui.html#typed-columns-and-named-view-settings\"" content "${content}")
  string(REPLACE "href=\"declarative-ui.md#scene-fine-settings\"" "href=\"declarative-ui.html#scene-fine-settings\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-recipe\"" "href=\"glossary.html#glossary-recipe\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-scene\"" "href=\"glossary.html#glossary-scene\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-ui-stage\"" "href=\"glossary.html#glossary-ui-stage\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-ownership\"" "href=\"glossary.html#glossary-ownership\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-text-units\"" "href=\"glossary.html#glossary-text-units\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-model-pin\"" "href=\"glossary.html#glossary-model-pin\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-backpressure\"" "href=\"glossary.html#glossary-backpressure\"" content "${content}")
  string(REPLACE "href=\"planned-features.md#planned-style-parameters\"" "href=\"planned-features.html#planned-style-parameters\"" content "${content}")
  string(REPLACE "href=\"planned-features.md#planned-themes\"" "href=\"planned-features.html#planned-themes\"" content "${content}")
  string(REPLACE "href=\"declarative-ui.md#pipe-click-handlers-ru\"" "href=\"declarative-ui-ru.html#pipe-click-handlers-ru\"" content "${content}")
  string(REPLACE "href=\"declarative-ui.md#short-key-bindings-ru\"" "href=\"declarative-ui-ru.html#short-key-bindings-ru\"" content "${content}")
  string(REPLACE "href=\"declarative-ui.md#scene-fine-settings-ru\"" "href=\"declarative-ui-ru.html#scene-fine-settings-ru\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-recipe-ru\"" "href=\"glossary-ru.html#glossary-recipe-ru\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-scene-ru\"" "href=\"glossary-ru.html#glossary-scene-ru\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-ui-stage-ru\"" "href=\"glossary-ru.html#glossary-ui-stage-ru\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-ownership-ru\"" "href=\"glossary-ru.html#glossary-ownership-ru\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-text-units-ru\"" "href=\"glossary-ru.html#glossary-text-units-ru\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-frame-ru\"" "href=\"glossary-ru.html#glossary-frame-ru\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-model-pin-ru\"" "href=\"glossary-ru.html#glossary-model-pin-ru\"" content "${content}")
  string(REPLACE "href=\"glossary.md#glossary-backpressure-ru\"" "href=\"glossary-ru.html#glossary-backpressure-ru\"" content "${content}")
  string(REPLACE "href=\"planned-features.md#planned-style-parameters-ru\"" "href=\"planned-features-ru.html#planned-style-parameters-ru\"" content "${content}")
  string(REPLACE "href=\"planned-features.md#planned-themes-ru\"" "href=\"planned-features-ru.html#planned-themes-ru\"" content "${content}")
  # END GENERATED FRAGMENT TARGETS
  file(WRITE "${page_file}" "${content}")
endforeach()

file(GLOB index_files "${HTML_DIR}/functions*.html")
foreach(index_file IN LISTS index_files)
  file(READ "${index_file}" original)
  set(content "${original}")

  # Doxygen encodes the destructor anchor but leaves its incoming links unencoded.
  string(REGEX MATCHALL "href=\"(functions[^\"]*\\.html)?#index_~\"" destructor_links "${content}")
  foreach(link IN LISTS destructor_links)
    string(REGEX REPLACE "^href=\"([^#]*)#index_~\"$" "\\1" target "${link}")
    if(target STREQUAL "")
      set(target_file "${index_file}")
    else()
      set(target_file "${HTML_DIR}/${target}")
    endif()
    if(EXISTS "${target_file}")
      file(READ "${target_file}" target_content)
      if(target_content MATCHES "(id|name)=\"index__7E\"")
        string(REPLACE "${link}" "href=\"${target}#index__7E\"" content "${content}")
      else()
        message(FATAL_ERROR "Unrecognized destructor anchor in ${target_file}; left unchanged")
      endif()
    else()
      message(FATAL_ERROR "Missing destructor index target ${target_file}; left unchanged")
    endif()
  endforeach()

  # Short member lists omit letter headings. Attach the missing local anchor to
  # the first actual member row, preserving the complete index and its links.
  string(REGEX MATCHALL "href=\"#index_[a-z]\"" letter_links "${content}")
  foreach(link IN LISTS letter_links)
    string(REGEX REPLACE "^href=\"#index_([a-z])\"$" "\\1" letter "${link}")
    if(content MATCHES "(id|name)=\"index_${letter}\"")
      continue()
    endif()

    if(NOT content MATCHES "<div class=\"qindex\">[^\n]*</div>[\r\n]+<ul>")
      message(FATAL_ERROR "Unrecognized flat member index in ${index_file}; left unchanged")
    endif()
    string(TOUPPER "${letter}" upper_letter)
    # Related-symbol names include operator punctuation and encoded template characters.
    # Stop before any link markup or next row, while retaining the exact displayed name.
    string(REGEX MATCH "<li>[${letter}${upper_letter}][^<\r\n]*&#160;:" row "${content}")
    if(row STREQUAL "")
      message(FATAL_ERROR "Missing member row for index_${letter} in ${index_file}; left unchanged")
    endif()
    # Replace only the first matching row prefix, not every symbol in that group.
    string(FIND "${content}" "${row}" position)
    string(SUBSTRING "${content}" 0 ${position} before)
    string(SUBSTRING "${content}" ${position} -1 after)
    string(REGEX REPLACE "^<li>" "<li id=\"index_${letter}\">" after "${after}")
    set(content "${before}${after}")
  endforeach()

  # Do not rewrite already-finalized files: repeated execution preserves bytes.
  if(NOT content STREQUAL original)
    file(WRITE "${index_file}" "${content}")
  endif()
endforeach()
