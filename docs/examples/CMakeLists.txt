cmake_minimum_required(VERSION 3.24)
project(PortalsGuideExamples LANGUAGES CXX)

add_subdirectory(../.. portals-build)

file(GLOB guide_sources CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/*.cpp")
foreach(source IN LISTS guide_sources)
  get_filename_component(name "${source}" NAME_WE)
  add_executable(${name} "${source}")
  target_link_libraries(${name} PRIVATE portals::core)
endforeach()
