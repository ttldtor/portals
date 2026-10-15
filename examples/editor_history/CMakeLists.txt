add_executable(portals_editor_history main.cpp)
target_link_libraries(portals_editor_history PRIVATE portals::core portals::lyra)
portals_enable_warnings(portals_editor_history)
