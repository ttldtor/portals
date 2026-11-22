# Optional native gzip adapter; ordinary core consumers need no compression runtime.
include("${CMAKE_CURRENT_SOURCE_DIR}/vendor/zlib/verify.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/vendor/zlib/symbols.cmake")

add_library(portals_zlib OBJECT
  vendor/zlib/adler32.c vendor/zlib/crc32.c vendor/zlib/inflate.c
  vendor/zlib/inffast.c vendor/zlib/inftrees.c vendor/zlib/zutil.c)
set_target_properties(portals_zlib PROPERTIES C_STANDARD 99 C_STANDARD_REQUIRED YES C_EXTENSIONS OFF
  POSITION_INDEPENDENT_CODE ON)
target_compile_definitions(portals_zlib PRIVATE ${portals_zlib_symbols})
target_include_directories(portals_zlib PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zlib")

add_library(portals_gzip STATIC src/models/gzip_document.cpp src/widgets/ui_gzip.cpp
  $<TARGET_OBJECTS:portals_zlib>)
add_library(portals::gzip ALIAS portals_gzip)
set_target_properties(portals_gzip PROPERTIES EXPORT_NAME gzip CXX_EXTENSIONS OFF)
target_link_libraries(portals_gzip PUBLIC portals::core)
target_include_directories(portals_gzip PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/zlib")
target_compile_definitions(portals_gzip PRIVATE ${portals_zlib_symbols})
portals_enable_warnings(portals_gzip)
install(TARGETS portals_gzip EXPORT PortalsTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}/$<CONFIG>")
install(FILES vendor/zlib/LICENSE
  DESTINATION "${CMAKE_INSTALL_DATADIR}/Portals/licenses" RENAME zlib-LICENSE.txt)
install(FILES vendor/zlib/manifest.json vendor/zlib/README.md
  DESTINATION "${CMAKE_INSTALL_DATADIR}/Portals/zlib")

if(PORTALS_BUILD_TESTS)
  add_executable(portals_gzip_document tests/gzip_document.cpp)
  target_include_directories(portals_gzip_document SYSTEM PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/doctest")
  target_link_libraries(portals_gzip_document PRIVATE portals::gzip)
  portals_enable_warnings(portals_gzip_document)
  add_test(NAME portals.gzip-document COMMAND portals_gzip_document)
  set_tests_properties(portals.gzip-document PROPERTIES TIMEOUT 60)
endif()
