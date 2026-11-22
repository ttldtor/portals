add_executable(portals_document_view_example main.cpp)
target_link_libraries(portals_document_view_example PRIVATE portals::core)
portals_enable_warnings(portals_document_view_example)

add_executable(portals_document_navigation_example navigation.cpp)
target_link_libraries(portals_document_navigation_example PRIVATE portals::core)
portals_enable_warnings(portals_document_navigation_example)

add_executable(portals_document_files_example files.cpp)
target_link_libraries(portals_document_files_example PRIVATE portals::lyra)
portals_enable_warnings(portals_document_files_example)
if(MSVC)
  target_sources(portals_document_files_example PRIVATE windows-utf8.manifest)
endif()

add_executable(portals_document_highlight_example highlight.cpp)
target_link_libraries(portals_document_highlight_example PRIVATE portals::core)
portals_enable_warnings(portals_document_highlight_example)
add_custom_command(TARGET portals_document_highlight_example POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${CMAKE_CURRENT_SOURCE_DIR}/orders.sql" "${CMAKE_CURRENT_SOURCE_DIR}/notes.md"
    "$<TARGET_FILE_DIR:portals_document_highlight_example>"
  VERBATIM)

if(PORTALS_ENABLE_GZIP)
  add_executable(portals_document_gzip_example gzip.cpp)
  target_link_libraries(portals_document_gzip_example PRIVATE portals::gzip)
  portals_enable_warnings(portals_document_gzip_example)
  add_custom_command(TARGET portals_document_gzip_example POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${CMAKE_CURRENT_SOURCE_DIR}/archive.sql" "${CMAKE_CURRENT_SOURCE_DIR}/archive.sql.gz"
      "$<TARGET_FILE_DIR:portals_document_gzip_example>"
    VERBATIM)
endif()
