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

# Complete before/after forms are consumers of the relocated public package only.
foreach(variant IN ITEMS before after declarative)
  add_executable(portals_form_${variant} form_${variant}.cpp)
  target_link_libraries(portals_form_${variant} PRIVATE portals::core)
  set_target_properties(portals_form_${variant} PROPERTIES CXX_EXTENSIONS OFF)
  if(WIN32)
    target_sources(portals_form_${variant} PRIVATE no_dialogs.cpp)
  endif()
  add_test(NAME consumer.form-${variant} COMMAND portals_form_${variant})
  set_tests_properties(consumer.form-${variant} PROPERTIES TIMEOUT 15)
endforeach()

add_test(NAME consumer.form-equivalence
  COMMAND "${CMAKE_COMMAND}"
    "-DBEFORE=$<TARGET_FILE:portals_form_before>"
    "-DAFTER=$<TARGET_FILE:portals_form_after>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/compare_forms.cmake")
add_test(NAME consumer.form-typed-error COMMAND portals_form_after --failure)
set_tests_properties(consumer.form-equivalence consumer.form-typed-error PROPERTIES TIMEOUT 30)

add_test(NAME consumer.form-declarative-equivalence
  COMMAND "${CMAKE_COMMAND}"
    "-DBEFORE=$<TARGET_FILE:portals_form_after>"
    "-DAFTER=$<TARGET_FILE:portals_form_declarative>"
    -P "${CMAKE_CURRENT_SOURCE_DIR}/compare_forms.cmake")
add_test(NAME consumer.form-declarative-typed-error COMMAND portals_form_declarative --failure)
set_tests_properties(consumer.form-declarative-equivalence consumer.form-declarative-typed-error PROPERTIES TIMEOUT 30)
