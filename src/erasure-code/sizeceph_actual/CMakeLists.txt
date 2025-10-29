# SizeCeph_Actual erasure code plugin - Production-Safe Implementation

set(sizeceph_actual_plugin_src
  ErasureCodePluginSizeCephActual.cc
  ErasureCodeSizeCephActual.cc)

add_library(sizeceph_actual_plugin_obj OBJECT ${sizeceph_actual_plugin_src})
target_link_libraries(sizeceph_actual_plugin_obj legacy-option-headers)

add_library(ec_sizeceph_actual SHARED $<TARGET_OBJECTS:sizeceph_actual_plugin_obj>)

# Link with sizeceph_actual library and required dependencies
target_link_libraries(ec_sizeceph_actual 
  ${CMAKE_DL_LIBS}
  # Note: sizeceph_actual library will be loaded dynamically
)

# Try to find sizeceph_actual library in standard locations
find_library(SIZECEPH_ACTUAL_LIBRARY
  NAMES sizeceph_actual libsizeceph_actual.so
  PATHS /usr/local/lib /usr/lib /opt/lib
  DOC "SizeCeph_Actual library"
)

if(SIZECEPH_ACTUAL_LIBRARY)
  message(STATUS "Found SizeCeph_Actual library: ${SIZECEPH_ACTUAL_LIBRARY}")
  target_link_libraries(ec_sizeceph_actual ${SIZECEPH_ACTUAL_LIBRARY})
else()
  message(WARNING "SizeCeph_Actual library not found - plugin will load dynamically")
  # Still build the plugin, but it will try to load dynamically
endif()

# Include sizeceph_actual headers if available
find_path(SIZECEPH_ACTUAL_INCLUDE_DIR
  NAMES sizeceph_actual.h
  PATHS /usr/local/include /usr/include /opt/include
  DOC "SizeCeph_Actual headers"
)

if(SIZECEPH_ACTUAL_INCLUDE_DIR)
  message(STATUS "Found SizeCeph_Actual headers: ${SIZECEPH_ACTUAL_INCLUDE_DIR}")
  target_include_directories(sizeceph_actual_plugin_obj PRIVATE ${SIZECEPH_ACTUAL_INCLUDE_DIR})
endif()

set_target_properties(ec_sizeceph_actual PROPERTIES
  INSTALL_RPATH ""
  VERSION 2.0.0
  SOVERSION 2
  CXX_VISIBILITY_PRESET hidden)

install(TARGETS ec_sizeceph_actual DESTINATION ${erasure_code_dir})