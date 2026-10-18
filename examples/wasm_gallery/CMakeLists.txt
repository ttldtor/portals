if(NOT EMSCRIPTEN)
  message(FATAL_ERROR "PORTALS_BUILD_WASM_GALLERY requires the Emscripten toolchain")
endif()

add_executable(portals_wasm_gallery main.cpp browser_terminal.cpp)
target_link_libraries(portals_wasm_gallery PRIVATE portals::core)
portals_enable_warnings(portals_wasm_gallery)
set_target_properties(portals_wasm_gallery PROPERTIES
  OUTPUT_NAME portals-gallery
  SUFFIX ".js"
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/wasm-gallery")
target_link_options(portals_wasm_gallery PRIVATE
  "-sMODULARIZE=1" "-sEXPORT_ES6=1" "-sEXPORT_NAME=createPortalsGallery"
  "-sENVIRONMENT=web,node" "-sALLOW_MEMORY_GROWTH=1" "-sNO_EXIT_RUNTIME=1"
  "-sEXPORTED_RUNTIME_METHODS=['HEAPU8']" "-sEXPORTED_FUNCTIONS=['_main','_malloc','_free']")

add_executable(portals_wasm_sdk_smoke sdk_smoke.cpp)
target_link_libraries(portals_wasm_sdk_smoke PRIVATE
  "-Wl,--whole-archive" portals::core "-Wl,--no-whole-archive")
portals_enable_warnings(portals_wasm_sdk_smoke)
set_target_properties(portals_wasm_sdk_smoke PROPERTIES SUFFIX ".js")
target_link_options(portals_wasm_sdk_smoke PRIVATE "-sENVIRONMENT=node")
