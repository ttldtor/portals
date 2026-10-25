add_executable(portals_unicode_panels main.cpp)
target_link_libraries(portals_unicode_panels PRIVATE portals::core portals::lyra)
portals_enable_warnings(portals_unicode_panels)
