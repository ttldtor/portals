cmake_minimum_required(VERSION 3.24)
project(PortalsPackageConsumer LANGUAGES CXX)

find_package(Portals 0.1 CONFIG REQUIRED)
cmake_path(IS_PREFIX CMAKE_PREFIX_PATH "${Portals_DIR}" NORMALIZE fromInstalledPrefix)
if(NOT fromInstalledPrefix)
  message(FATAL_ERROR "Portals was found outside the relocated prefix: ${Portals_DIR}")
endif()

# Both archives must be imported from the installation, with no private build settings.
foreach(target IN ITEMS portals::core portals::utf8proc)
  get_target_property(imported ${target} IMPORTED)
  if(NOT imported)
    message(FATAL_ERROR "${target} is not an installed imported target")
  endif()
  foreach(property IN ITEMS INTERFACE_COMPILE_OPTIONS INTERFACE_COMPILE_DEFINITIONS)
    get_target_property(value ${target} ${property})
    if(value)
      message(FATAL_ERROR "Private build settings leaked through ${target}: ${property}=${value}")
    endif()
  endforeach()
endforeach()

add_executable(portals_package_consumer main.cpp)
target_link_libraries(portals_package_consumer PRIVATE portals::core)
set_target_properties(portals_package_consumer PROPERTIES CXX_EXTENSIONS OFF)
if(WIN32)
  # The install test stages the existing process startup source beside this fixture.
  target_sources(portals_package_consumer PRIVATE no_dialogs.cpp)
endif()

enable_testing()
add_test(NAME consumer.model-widget COMMAND portals_package_consumer)
set_tests_properties(consumer.model-widget PROPERTIES TIMEOUT 15)
